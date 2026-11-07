/* vk_comprobar.c — ¿hay Vulkan en este PC? (lo mira el launcher antes de preparar el juego).
 *
 * Sin Vulkan el port no dibuja nada: antes se veía solo una ventana negra (portátil con gráficos
 * Intel y el driver del fabricante). Aquí se carga vulkan-1.dll, se crea una instancia 1.1 y se
 * listan las tarjetas; el resultado va al registro y, si falla, a un aviso con qué hacer. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* 1 = hay una tarjeta con Vulkan 1.1 o más; 0 = no (motivo en `por_que`). */
int vk_hay_dedicada;   /* tras vk_comprobar: 1 si hay una tarjeta dedicada (NVIDIA/AMD) */
int vk_comprobar(char *por_que, size_t max, char *tarjeta, size_t tmax) {
    if (tarjeta && tmax) tarjeta[0] = 0;
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { snprintf(por_que, max, "no está instalado Vulkan (falta vulkan-1.dll)"); return 0; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    if (!ci) { snprintf(por_que, max, "vulkan-1.dll no es válida"); return 0; }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Project Baseball (comprobación)";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = ci(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { snprintf(por_que, max, "el driver no pudo iniciar Vulkan (error %d)", (int)r); return 0; }
    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    uint32_t n = 0;
    VkPhysicalDevice dev[8];
    if (epd) { epd(inst, &n, NULL); if (n > 8) n = 8; epd(inst, &n, dev); }
    int ok = 0;
    char lista[512] = "";
    for (uint32_t i = 0; i < n && gpp; i++) {
        VkPhysicalDeviceProperties p;
        gpp(dev[i], &p);
        uint32_t v = p.apiVersion;
        printf("[vulkan] tarjeta %u: %s (Vulkan %u.%u.%u, driver 0x%X)\n", i, p.deviceName, VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v),
               VK_VERSION_PATCH(v), p.driverVersion);
        { extern void arranque(const char *fmt, ...);
          arranque("vulkan: tarjeta %u: %s (Vulkan %u.%u.%u, driver 0x%X, tipo %d)", i, p.deviceName, VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v),
                   VK_VERSION_PATCH(v), p.driverVersion, (int)p.deviceType); }
        size_t l = strlen(lista);
        snprintf(lista + l, sizeof(lista) - l, "%s%s (Vulkan %u.%u)", l ? ", " : "", p.deviceName, VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v));
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) vk_hay_dedicada = 1;
        if (VK_VERSION_MAJOR(v) > 1 || VK_VERSION_MINOR(v) >= 1) {
            ok = 1;
            if (tarjeta && tmax && !tarjeta[0]) snprintf(tarjeta, tmax, "%s", p.deviceName);
        }
    }
    if (di) di(inst, NULL);
    fflush(stdout);
    if (!n) { snprintf(por_que, max, "Vulkan no encuentra ninguna tarjeta gráfica"); return 0; }
    if (!ok) { snprintf(por_que, max, "la tarjeta solo tiene una versión antigua de Vulkan: %s", lista); return 0; }
    return 1;
}
