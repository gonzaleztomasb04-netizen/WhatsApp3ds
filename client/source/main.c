// Cliente WhatsApp para 3DS (devkitARM + libctru + citro2d)
// Encuentra el puente solo (broadcast UDP), no hace falta escribir la IP.
// Controles (o toca los botones de la pantalla de abajo):
//  Lista:  Arriba/Abajo mover, A abrir, Y actualizar, START salir
//  Chat:   X escribir mensaje, Y actualizar, B volver (se actualiza solo cada ~5 s)
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// ---- CONFIGURA ESTO ----
#define TOKEN "tomol573"
// ------------------------

#define DISCOVERY_PORT 8081
#define BUFSZ 32768
#define MAXCH 30
#define MAXMSG 14
#define MAXLINES 8
#define LINELEN 64

#define HEADER_H 30.0f
#define ROW_H 30.0f
#define LINE_H 16.0f
#define PAD_X 8.0f
#define PAD_Y 5.0f
#define TSCALE 0.5f

#define C_HEADER C2D_Color32(7, 94, 84, 255)
#define C_GREEN  C2D_Color32(37, 211, 102, 255)
#define C_BG     C2D_Color32(236, 229, 221, 255)
#define C_MINE   C2D_Color32(220, 248, 198, 255)
#define C_WHITE  C2D_Color32(255, 255, 255, 255)
#define C_ROW2   C2D_Color32(247, 247, 247, 255)
#define C_SEL    C2D_Color32(206, 236, 224, 255)
#define C_TEXT   C2D_Color32(25, 25, 25, 255)
#define C_GRAY   C2D_Color32(110, 120, 125, 255)
#define C_GRAY2  C2D_Color32(122, 138, 147, 255)
#define C_DIV    C2D_Color32(225, 225, 225, 255)
#define C_PANEL  C2D_Color32(240, 242, 245, 255)

enum { ACT_NONE = 0, ACT_UP, ACT_DOWN, ACT_OPEN, ACT_REFRESH, ACT_EXIT, ACT_WRITE, ACT_BACK };

typedef struct {
    char id[64];
    char name[48];
    int unread;
} Chat;

typedef struct {
    int me;
    char text[200];
} Msg;

typedef struct {
    float x, y, w, h;
    const char *label;
    int act;
} Button;

static const Button list_btns[3] = {
    {30, 34, 260, 46, "Abrir chat  (A)", ACT_OPEN},
    {30, 94, 260, 46, "Actualizar  (Y)", ACT_REFRESH},
    {30, 154, 260, 46, "Salir  (START)", ACT_EXIT},
};
static const Button chat_btns[3] = {
    {30, 34, 260, 46, "Escribir  (X)", ACT_WRITE},
    {30, 94, 260, 46, "Actualizar  (Y)", ACT_REFRESH},
    {30, 154, 260, 46, "Volver  (B)", ACT_BACK},
};

static Chat chats[MAXCH];
static int nchats = 0;
static Msg msgs[MAXMSG];
static int nmsgs = 0;
static char buf[BUFSZ];
static char status[64] = "";
static char bridge_url[64] = "";
static int view = 0; // 0 = lista, 1 = chat
static int sel = 0;

static C3D_RenderTarget *top, *bot;
static C2D_TextBuf tbuf;

// ---------------- Red ----------------

// Busca el puente en la red local con un aviso UDP a toda la red.
static int discover(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return 0;

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DISCOVERY_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    for (int tries = 0; tries < 3; tries++) {
        sendto(s, "WA3DS?", 6, 0, (struct sockaddr *)&dst, sizeof dst);
        for (int w = 0; w < 30; w++) {
            char reply[32];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(s, reply, sizeof reply - 1, 0, (struct sockaddr *)&from, &fl);
            if (n >= 7 && memcmp(reply, "WA3DS:", 6) == 0) {
                reply[n] = 0;
                snprintf(bridge_url, sizeof bridge_url, "http://%s:%d",
                         inet_ntoa(from.sin_addr), atoi(reply + 6));
                close(s);
                return 1;
            }
            svcSleepThread(16666666LL);
        }
    }
    close(s);
    return 0;
}

// Devuelve bytes recibidos (>=0), -2 si no hay puente, u otro negativo si falla (-codigo HTTP o -1).
static int http(int post, const char *path, const char *body)
{
    if (!discover()) return -2;

    char url[360];
    snprintf(url, sizeof url, "%s%s", bridge_url, path);

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

static void set_error(const char *what, int n)
{
    if (n == -2) snprintf(status, sizeof status, "Puente no encontrado en esta red");
    else snprintf(status, sizeof status, "%s (%d)", what, n);
}

static void load_chats(void)
{
    nchats = 0;
    int n = http(0, "/chats", NULL);
    if (n < 0) {
        set_error("Error del puente", n);
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
    if (sel >= nchats) sel = nchats > 0 ? nchats - 1 : 0;
}

static void load_msgs(void)
{
    char path[200];
    snprintf(path, sizeof path, "/messages?chat=%s&limit=12", chats[sel].id);
    int n = http(0, path, NULL);
    if (n < 0) {
        set_error("Error al cargar mensajes", n);
        return;
    }
    status[0] = 0;
    nmsgs = 0;
    char *save;
    for (char *l = strtok_r(buf, "\n", &save); l && nmsgs < MAXMSG; l = strtok_r(NULL, "\n", &save)) {
        char *t = strchr(l, '\t');
        if (!t) continue;
        *t++ = 0;
        msgs[nmsgs].me = (l[0] == '1');
        snprintf(msgs[nmsgs].text, sizeof msgs[nmsgs].text, "%s", t);
        nmsgs++;
    }
}

// ---------------- Dibujo ----------------

static void draw_text(const char *s, float x, float y, float z, float scale, u32 color)
{
    C2D_Text t;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, z, scale, scale, color);
}

static void rounded(float x, float y, float w, float h, u32 color, float z)
{
    float r = 8.0f;
    C2D_DrawRectSolid(x + r, y, z, w - 2 * r, h, color);
    C2D_DrawRectSolid(x, y + r, z, w, h - 2 * r, color);
    C2D_DrawCircleSolid(x + r, y + r, z, r, color);
    C2D_DrawCircleSolid(x + w - r, y + r, z, r, color);
    C2D_DrawCircleSolid(x + r, y + h - r, z, r, color);
    C2D_DrawCircleSolid(x + w - r, y + h - r, z, r, color);
}

static void draw_header(const char *title)
{
    C2D_DrawRectSolid(0, 0, 0.5f, 400, HEADER_H, C_HEADER);
    draw_text(title, 12, 6, 0.6f, 0.6f, C_WHITE);
}

// Parte el texto en lineas de hasta maxchars caracteres, cortando en espacios.
static int wrap_text(const char *s, char lines[][LINELEN], int maxchars)
{
    int n = 0;
    while (*s && n < MAXLINES) {
        int len = (int)strlen(s);
        int cut = len;
        if (len > maxchars) {
            cut = maxchars;
            int sp = -1;
            for (int i = cut; i > 0; i--) {
                if (s[i] == ' ') { sp = i; break; }
            }
            if (sp > 0) cut = sp;
            else while (cut > 0 && (((unsigned char)s[cut]) & 0xC0) == 0x80) cut--;
        }
        if (cut <= 0) cut = 1;
        if (cut >= LINELEN) cut = LINELEN - 1;
        memcpy(lines[n], s, cut);
        lines[n][cut] = 0;
        n++;
        s += cut;
        while (*s == ' ') s++;
    }
    if (n == 0) {
        lines[0][0] = 0;
        n = 1;
    }
    return n;
}

static u32 avatar_color(char c)
{
    switch (((unsigned char)c) % 5) {
        case 0: return C2D_Color32(0, 150, 136, 255);
        case 1: return C2D_Color32(63, 81, 181, 255);
        case 2: return C2D_Color32(233, 30, 99, 255);
        case 3: return C2D_Color32(255, 152, 0, 255);
        default: return C2D_Color32(103, 58, 183, 255);
    }
}

static void draw_list_top(void)
{
    draw_header("WhatsApp");

    if (nchats == 0) {
        draw_text("Sin chats todavia.", 100, 100, 0.3f, 0.6f, C_GRAY);
        draw_text("Envia o recibe un mensaje y actualiza (Y).", 40, 125, 0.3f, 0.45f, C_GRAY);
        return;
    }

    int first = sel - 3;
    if (first < 0) first = 0;
    for (int r = 0; r < 7 && first + r < nchats; r++) {
        int i = first + r;
        float y = HEADER_H + r * ROW_H;
        u32 bg = (i == sel) ? C_SEL : ((r % 2) ? C_ROW2 : C_WHITE);
        C2D_DrawRectSolid(0, y, 0.1f, 400, ROW_H, bg);
        if (i == sel) C2D_DrawRectSolid(0, y, 0.2f, 4, ROW_H, C_GREEN);
        C2D_DrawRectSolid(44, y + ROW_H - 1, 0.2f, 356, 1, C_DIV);

        char c = chats[i].name[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 33 || c > 126) c = '#';
        char ini[2] = {c, 0};
        C2D_DrawCircleSolid(26, y + ROW_H / 2, 0.2f, 11, avatar_color(chats[i].name[0]));
        draw_text(ini, 21, y + 6, 0.3f, 0.5f, C_WHITE);

        draw_text(chats[i].name, 44, y + 6, 0.3f, 0.55f, C_TEXT);

        if (chats[i].unread > 0) {
            char num[8];
            snprintf(num, sizeof num, "%d", chats[i].unread > 99 ? 99 : chats[i].unread);
            C2D_DrawCircleSolid(378, y + ROW_H / 2, 0.2f, 10, C_GREEN);
            draw_text(num, chats[i].unread > 9 ? 372 : 375, y + 8, 0.3f, 0.45f, C_WHITE);
        }
    }
}

static void draw_chat_top(void)
{
    if (nmsgs == 0) {
        draw_text("Sin mensajes.", 135, 110, 0.3f, 0.6f, C_GRAY);
    }

    float yb = 240.0f - 6.0f;
    for (int i = nmsgs - 1; i >= 0; i--) {
        char lines[MAXLINES][LINELEN];
        int nl = wrap_text(msgs[i].text, lines, 38);
        float h = nl * LINE_H + 2 * PAD_Y;
        float y = yb - h;

        C2D_Text t[MAXLINES];
        float wmax = 0;
        for (int k = 0; k < nl; k++) {
            C2D_TextParse(&t[k], tbuf, lines[k]);
            C2D_TextOptimize(&t[k]);
            float w, hh;
            C2D_TextGetDimensions(&t[k], TSCALE, TSCALE, &w, &hh);
            if (w > wmax) wmax = w;
        }
        float bw = wmax + 2 * PAD_X;
        float x = msgs[i].me ? 400.0f - 8.0f - bw : 8.0f;
        rounded(x, y, bw, h, msgs[i].me ? C_MINE : C_WHITE, 0.2f);
        for (int k = 0; k < nl; k++)
            C2D_DrawText(&t[k], C2D_WithColor, x + PAD_X, y + PAD_Y + k * LINE_H, 0.3f, TSCALE, TSCALE, C_TEXT);

        yb = y - 5.0f;
        if (y < HEADER_H) break;
    }

    draw_header(nchats > 0 ? chats[sel].name : "Chat");
}

static u32 btn_color(int act)
{
    if (act == ACT_OPEN || act == ACT_WRITE) return C_GREEN;
    if (act == ACT_REFRESH) return C_HEADER;
    return C_GRAY2;
}

static void draw_bottom(void)
{
    draw_text("WhatsApp 3DS", 10, 6, 0.3f, 0.5f, C_GRAY);

    const Button *b = (view == 0) ? list_btns : chat_btns;
    for (int i = 0; i < 3; i++) {
        rounded(b[i].x, b[i].y, b[i].w, b[i].h, btn_color(b[i].act), 0.2f);
        C2D_Text t;
        float w, h;
        C2D_TextParse(&t, tbuf, b[i].label);
        C2D_TextOptimize(&t);
        C2D_TextGetDimensions(&t, 0.6f, 0.6f, &w, &h);
        C2D_DrawText(&t, C2D_WithColor, b[i].x + (b[i].w - w) / 2, b[i].y + (b[i].h - h) / 2,
                     0.3f, 0.6f, 0.6f, C_WHITE);
    }

    if (status[0]) draw_text(status, 10, 214, 0.3f, 0.45f, C_GRAY);
}

static void render(void)
{
    C2D_TextBufClear(tbuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    C2D_TargetClear(top, C_BG);
    C2D_SceneBegin(top);
    if (view == 0) draw_list_top();
    else draw_chat_top();

    C2D_TargetClear(bot, C_PANEL);
    C2D_SceneBegin(bot);
    draw_bottom();

    C3D_FrameEnd(0);
}

// Muestra un mensaje en pantalla antes de una operacion lenta de red.
static void show_loading(const char *msg)
{
    snprintf(status, sizeof status, "%s", msg);
    render();
}

// ---------------- Acciones ----------------

static void send_message(void)
{
    static char text[256];
    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, 255);
    swkbdSetHintText(&swkbd, "Mensaje");
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    text[0] = 0;
    SwkbdButton b = swkbdInputText(&swkbd, text, sizeof text);
    if (b != SWKBD_BUTTON_RIGHT) return;

    show_loading("Enviando...");
    static char body[360];
    snprintf(body, sizeof body, "%s\n%s", chats[sel].id, text);
    int n = http(1, "/send", body);
    if (n >= 0) snprintf(status, sizeof status, "Enviado");
    else set_error("Error al enviar", n);
}

static int hit_test(const Button *b, int px, int py)
{
    for (int i = 0; i < 3; i++) {
        if (px >= b[i].x && px <= b[i].x + b[i].w && py >= b[i].y && py <= b[i].y + b[i].h)
            return b[i].act;
    }
    return ACT_NONE;
}

int main(void)
{
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(8192);
    httpcInit(0);

    u32 *soc_buf = (u32 *)memalign(0x1000, 0x20000);
    if (soc_buf) socInit(soc_buf, 0x20000);

    int frames = 0;
    show_loading("Buscando el puente...");
    load_chats();

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();

        int act = ACT_NONE;
        if (view == 0) {
            if (down & KEY_START) act = ACT_EXIT;
            else if (down & KEY_DOWN) act = ACT_DOWN;
            else if (down & KEY_UP) act = ACT_UP;
            else if (down & KEY_A) act = ACT_OPEN;
            else if (down & KEY_Y) act = ACT_REFRESH;
        } else {
            if (down & KEY_B) act = ACT_BACK;
            else if (down & KEY_X) act = ACT_WRITE;
            else if (down & KEY_Y) act = ACT_REFRESH;
        }
        if (down & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            int t = hit_test(view == 0 ? list_btns : chat_btns, tp.px, tp.py);
            if (t != ACT_NONE) act = t;
        }

        if (act == ACT_EXIT) break;

        if (view == 0) {
            if (act == ACT_DOWN && sel < nchats - 1) sel++;
            else if (act == ACT_UP && sel > 0) sel--;
            else if (act == ACT_REFRESH) {
                show_loading("Actualizando...");
                load_chats();
            } else if (act == ACT_OPEN && nchats > 0) {
                view = 1;
                nmsgs = 0;
                frames = 0;
                show_loading("Cargando mensajes...");
                load_msgs();
            }
        } else {
            if (act == ACT_BACK) {
                view = 0;
                show_loading("Actualizando...");
                load_chats();
            } else if (act == ACT_WRITE) {
                send_message();
                load_msgs();
                frames = 0;
            } else if (act == ACT_REFRESH) {
                show_loading("Actualizando...");
                load_msgs();
                frames = 0;
            } else if (++frames >= 300) {
                load_msgs();
                frames = 0;
            }
        }

        render();
    }

    socExit();
    free(soc_buf);
    httpcExit();
    C2D_TextBufDelete(tbuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
