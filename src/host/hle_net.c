/* Red (SceNet, SceNetCtl, SceHttp, SceSsl, SceNp*) para el port de MLB 15.
 *
 * El juego inicializa la pila de red al arrancar (online, PSN, trofeos...).
 * En el PC no hay red de la Vita, asi que se responde como una consola
 * DESCONECTADA: las funciones de inicializacion/terminacion devuelven 0 para
 * que el juego pueda seguir su flujo normal, y las operaciones reales
 * (sockets, HTTP, PSN) devuelven error. El juego debe caer por la rama
 * "sin conexion" y continuar en modo offline.
 *
 * Todo esto es deliberadamente superficial: si algun dia se quiere online de
 * verdad, se sustituye por una implementacion sobre Winsock/HTTP de Windows.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include "port.h"

#define NET_ERROR ((uint32_t)-1)

static int g_trace = -1;
static int trace_on(void) {
    if (g_trace < 0) g_trace = getenv("MLB15_NET_TRACE") ? 1 : 0;
    return g_trace;
}

#define ONCE(name) do { static int _o; if (!_o) { _o = 1; \
    printf("[net] %s(r0=0x%X r1=0x%X r2=0x%X r3=0x%X)\n", name, r0, r1, r2, r3); \
    fflush(stdout); } } while (0)

/* --- SceNet --------------------------------------------------------------- */

void vita_hle_sceNetInit(void) {
    ONCE("sceNetInit");
    /* SceNetInitParam: no hace falta leerlo, solo aceptar. */
    r0 = 0;
}
void vita_hle_sceNetTerm(void) { r0 = 0; }

void vita_hle_sceNetErrnoLoc(void) {
    static uint32_t errno_guest;
    if (!errno_guest) errno_guest = port_alloc(4, 4, "net errno");
    r0 = errno_guest;
}

/* Sockets: no hay conexion, cualquier operacion falla. */
void vita_hle_sceNetSocket(void) { r0 = NET_ERROR; }
void vita_hle_sceNetSocketClose(void) { r0 = NET_ERROR; }
void vita_hle_sceNetSocketAbort(void) { r0 = NET_ERROR; }
void vita_hle_sceNetBind(void) { r0 = NET_ERROR; }
void vita_hle_sceNetConnect(void) { r0 = NET_ERROR; }
void vita_hle_sceNetListen(void) { r0 = NET_ERROR; }
void vita_hle_sceNetAccept(void) { r0 = NET_ERROR; }
void vita_hle_sceNetSend(void) { r0 = NET_ERROR; }
void vita_hle_sceNetSendto(void) { r0 = NET_ERROR; }
void vita_hle_sceNetRecv(void) { r0 = NET_ERROR; }
void vita_hle_sceNetRecvfrom(void) { r0 = NET_ERROR; }
void vita_hle_sceNetSetsockopt(void) { r0 = NET_ERROR; }
void vita_hle_sceNetResolverCreate(void) { r0 = NET_ERROR; }
void vita_hle_sceNetResolverDestroy(void) { r0 = NET_ERROR; }
void vita_hle_sceNetResolverStartNtoa(void) { r0 = NET_ERROR; }

/* Conversiones de orden de bytes: estas si son reales (no dependen de red) y
 * el juego podria usarlas para dar formato a datos locales. */
void vita_hle_sceNetHtons(void) { r0 = ((r0 & 0xFFu) << 8) | ((r0 >> 8) & 0xFFu); }
void vita_hle_sceNetNtohs(void) { vita_hle_sceNetHtons(); }
void vita_hle_sceNetHtonl(void) {
    r0 = ((r0 & 0x000000FFu) << 24) | ((r0 & 0x0000FF00u) << 8) |
         ((r0 & 0x00FF0000u) >> 8)  | ((r0 & 0xFF000000u) >> 24);
}
void vita_hle_sceNetNtohl(void) { vita_hle_sceNetHtonl(); }

/* --- SceNetCtl ------------------------------------------------------------ */

void vita_hle_sceNetCtlInit(void) { ONCE("sceNetCtlInit"); r0 = 0; }
void vita_hle_sceNetCtlTerm(void) { r0 = 0; }
void vita_hle_sceNetCtlCheckCallback(void) { r0 = 0; }

void vita_hle_sceNetCtlInetGetState(void) {
    /* r0 = int* state; SCE_NETCTL_STATE_DISCONNECTED = 0 */
    if (vita_mem_ptr(r0, 4)) vita_write32(r0, 0);
    ONCE("sceNetCtlInetGetState -> desconectado");
    r0 = 0;
}

void vita_hle_sceNetCtlInetGetInfo(void) {
    /* No hay informacion de conexion que dar. */
    r0 = NET_ERROR;
}
void vita_hle_sceNetCtlGetNatInfo(void) { r0 = NET_ERROR; }

void vita_hle_sceNetCtlInetRegisterCallback(void) {
    /* r0 = funcion, r1 = arg, r2 = int* id */
    if (r2 && vita_mem_ptr(r2, 4)) vita_write32(r2, 1);
    r0 = 0;
}
void vita_hle_sceNetCtlInetUnregisterCallback(void) { r0 = 0; }

/* --- SceHttp -------------------------------------------------------------- */

void vita_hle_sceHttpInit(void) { r0 = 0; }
void vita_hle_sceHttpTerm(void) { r0 = 0; }
void vita_hle_sceHttpCreateTemplate(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpDeleteTemplate(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpCreateConnectionWithURL(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpDeleteConnection(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpCreateRequestWithURL(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpDeleteRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpSendRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpReadData(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpGetStatusCode(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpGetAllResponseHeaders(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpParseResponseHeader(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpGetResponseContentLength(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpAddRequestHeader(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpSetCookieEnabled(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpSetCookieRecvCallback(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpGetCookie(void) { r0 = NET_ERROR; }
void vita_hle_sceHttpsDisableOption(void) { r0 = NET_ERROR; }

/* --- SceSsl --------------------------------------------------------------- */

void vita_hle_sceSslInit(void) { r0 = 0; }
void vita_hle_sceSslTerm(void) { r0 = 0; }

/* --- SceNp* --------------------------------------------------------------- */
/* Init/Term devuelven 0 (el juego cierra lo que abre); todo lo que requiere
 * sesion con PSN devuelve error, que es el estado real sin conexion. */

void vita_hle_sceNpInit(void) { ONCE("sceNpInit (offline)"); r0 = 0; }
void vita_hle_sceNpTerm(void) { r0 = 0; }
void vita_hle_sceNpCheckCallback(void) { r0 = 0; }
void vita_hle_sceNpGetServiceState(void) { r0 = NET_ERROR; }
void vita_hle_sceNpManagerGetNpId(void) { r0 = NET_ERROR; }
void vita_hle_sceNpManagerGetCachedParam(void) { r0 = NET_ERROR; }
void vita_hle_sceNpManagerGetChatRestrictionFlag(void) { r0 = NET_ERROR; }
void vita_hle_sceNpManagerGetContentRatingFlag(void) { r0 = NET_ERROR; }
void vita_hle_sceNpRegisterServiceStateCallback(void) { r0 = NET_ERROR; }

void vita_hle_sceNpLookupInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupCreateTitleCtx(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupCreateRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupDeleteRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupPollAsync(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupNpIdAsync(void) { r0 = NET_ERROR; }
void vita_hle_sceNpLookupUserProfileAsync(void) { r0 = NET_ERROR; }

void vita_hle_sceNpAuthInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpAuthTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpAuthCreateStartRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceNpAuthDestroyRequest(void) { r0 = NET_ERROR; }
void vita_hle_sceNpAuthGetTicket(void) { r0 = NET_ERROR; }
void vita_hle_sceNpAuthGetEntitlementIdList(void) { r0 = NET_ERROR; }

void vita_hle_sceNpBasicInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpBasicTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpBasicCheckCallback(void) { r0 = NET_ERROR; }
void vita_hle_sceNpBasicRegisterHandler(void) { r0 = NET_ERROR; }
void vita_hle_sceNpBasicUnregisterHandler(void) { r0 = NET_ERROR; }
void vita_hle_sceNpBasicSetInGamePresence(void) { r0 = NET_ERROR; }
void vita_hle_sceNpBasicSendInGameDataMessage(void) { r0 = NET_ERROR; }

void vita_hle_sceNpMessageInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpMessageTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpMessageGetAttachedData(void) { r0 = NET_ERROR; }
void vita_hle_sceNpMessageSetAttachedDataUsedFlag(void) { r0 = NET_ERROR; }

void vita_hle_sceNpActivityInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpActivityPostStatus(void) { r0 = NET_ERROR; }

void vita_hle_sceNpCommerce2Init(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2Term(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateCtx(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2DestroyCtx(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateSessionStart(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateSessionGetResult(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2CreateSessionCreateReq(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2DestroyReq(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryContentsStart(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryContentsGetResult(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryContentsCreateReq(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2InitGetCategoryContentsResult(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetCategoryInfoFromContentInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetContentInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetGameProductInfoFromContentInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetGameSkuInfoFromGameProductInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetPrice(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2GetSessionInfo(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2ShowPsStoreIcon(void) { r0 = NET_ERROR; }
void vita_hle_sceNpCommerce2HidePsStoreIcon(void) { r0 = NET_ERROR; }

void vita_hle_sceNpTrophyInit(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyTerm(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyCreateHandle(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyCreateContext(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyDestroyHandle(void) { r0 = NET_ERROR; }
void vita_hle_sceNpTrophyUnlockTrophy(void) { r0 = NET_ERROR; }
