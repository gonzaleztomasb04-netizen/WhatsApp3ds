// Cliente WhatsApp para 3DS (devkitARM + libctru)
// Controles:
//  Lista:  Arriba/Abajo mover, A abrir, Y actualizar, START salir
//  Chat:   X escribir mensaje, Y actualizar, B volver (se actualiza solo cada ~5 s)
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- CONFIGURA ESTO ----
#define BRIDGE "http://192.168.100.9:8080"
#define TOKEN  "tomol573"
// ------------------------

#define BUFSZ (32 * 1024)
#define MAXCH 30

typedef struct {
    char id[64];
    char name[48];
    int unread;
} Chat;

static Chat chats[MAXCH];
static int nchats = 0;
static char buf[BUFSZ];
static char status[64] = "";

// Devuelve bytes recibidos (>=0) o negativo si falla (-codigo HTTP o -1).
static int http(int post, const char *path, const char *body)
{
    char url[320];
    snprintf(url, sizeof url, "%s%s", BRIDGE, path);

    httpcContext c;
    if (R_FAILED(httpcOpenContext(&c, post ? HTTPC_METHOD_POST : HTTPC_METHOD_GET, url, 1)))
        return -1;

    httpcAddRequestHeaderField(&c, "X-Token", TOKEN);
    httpcAddRequestHeaderField(&c, "User-Agent", "3ds-wa/1.0");
    if (post) {
        httpcAddRequestHeaderField(&c, "Content-Type", "text/plain");
        httpcAddPostDataRaw(&c, (const u32 *)body, strlen(body));
    }

    if (R_FAILED(httpcBeginRequest(&c))) {
        httpcCloseContext(&c);
        return -1;
    }

    u32 st = 0;
    httpcGetResponseStatusCode(&c, &st);

    u32 total = 0, got = 0;
    Result r;
    do {
        r = httpcDownloadData(&c, (u8 *)buf + total, BUFSZ - 1 - total, &got);
        total += got;
    } while (r == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING && total < BUFSZ - 1);
    buf[total] = 0;

    httpcCloseContext(&c);
    return st == 200 ? (int)total : -(int)st;
}

static void load_chats(void)
{
    nchats = 0;
    int n = http(0, "/chats", NULL);
    if (n < 0) {
        snprintf(status, sizeof status, "Error al cargar chats (%d)", n);
        return;
    }
    status[0] = 0;
    char *save;
    for (char *l = strtok_r(buf, "\n", &save); l && nchats < MAXCH; l = strtok_r(NULL, "\n", &save)) {
        char *t1 = strchr(l, '\t');
        if (!t1) continue;
        *t1++ = 0;
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = 0;
        Chat *c = &chats[nchats++];
        snprintf(c->id, sizeof c->id, "%s", l);
        snprintf(c->name, sizeof c->name, "%s", t1);
        c->unread = atoi(t2);
    }
}

static void draw_list(int sel)
{
    consoleClear();
    printf("WhatsApp 3DS\n");
    printf("Arriba/Abajo  A:abrir  Y:actualizar  START:salir\n");
    printf("--------------------------------------------------\n");
    int start = sel - 10;
    if (start < 0) start = 0;
    for (int i = start; i < nchats && i < start + 22; i++) {
        printf("%c %.38s", i == sel ? '>' : ' ', chats[i].name);
        if (chats[i].unread > 0) printf(" (%d)", chats[i].unread);
        printf("\n");
    }
    if (status[0]) printf("\n%s\n", status);
}

static void draw_chat(const Chat *c)
{
    char path[200];
    snprintf(path, sizeof path, "/messages?chat=%s&limit=12", c->id);
    int n = http(0, path, NULL);

    consoleClear();
    printf("== %.40s ==\n", c->name);
    printf("X:escribir  Y:actualizar  B:volver\n");
    printf("--------------------------------------------------\n");
    if (n < 0) {
        printf("Error al cargar mensajes (%d)\n", n);
        return;
    }
    char *save;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *t = strchr(l, '\t');
        if (!t) continue;
        *t++ = 0;
        printf("%s %s\n", l[0] == '1' ? "Yo:" : ">", t);
    }
    if (status[0]) printf("\n%s\n", status);
}

static void send_message(const Chat *c)
{
    static char text[256];
    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, 255);
    swkbdSetHintText(&swkbd, "Mensaje");
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    text[0] = 0;
    SwkbdButton b = swkbdInputText(&swkbd, text, sizeof text);
    if (b != SWKBD_BUTTON_RIGHT) return;

    static char body[360];
    snprintf(body, sizeof body, "%s\n%s", c->id, text);
    int n = http(1, "/send", body);
    snprintf(status, sizeof status, n >= 0 ? "Enviado" : "Error al enviar (%d)", n);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    httpcInit(0);

    int view = 0, sel = 0, frames = 0;
    load_chats();
    draw_list(sel);

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();

        if (view == 0) {
            if (down & KEY_START) break;
            if ((down & KEY_DOWN) && sel < nchats - 1) { sel++; draw_list(sel); }
            if ((down & KEY_UP) && sel > 0)            { sel--; draw_list(sel); }
            if (down & KEY_Y) { load_chats(); if (sel >= nchats) sel = 0; draw_list(sel); }
            if ((down & KEY_A) && nchats > 0) {
                view = 1; frames = 0; status[0] = 0;
                draw_chat(&chats[sel]);
            }
        } else {
            if (down & KEY_B) { view = 0; load_chats(); if (sel >= nchats) sel = 0; draw_list(sel); }
            else if (down & KEY_X) { send_message(&chats[sel]); draw_chat(&chats[sel]); frames = 0; }
            else if ((down & KEY_Y) || ++frames >= 300) { status[0] = 0; draw_chat(&chats[sel]); frames = 0; }
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    httpcExit();
    gfxExit();
    return 0;
}
