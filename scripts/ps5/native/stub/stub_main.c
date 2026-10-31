/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ProsperoBrowser native-title loader stub.
 *
 * This is a placeholder eboot.bin so the PRSB00001 tile installs, registers
 * through ShadowMountPlus and launches from the home screen today, while the
 * browser itself is not yet relinked as a native application (see
 * docs/native-title.md, "What the frontend/spawner must change"). It is a real
 * native PS5 app (entry _start from the vendored app_crt, process-parameter
 * block, libc.prx companion), not a payload.
 *
 * On launch it writes one line to the kernel log and raises a notification
 * that explains the state, then returns so the title closes cleanly. It does
 * NOT attempt to spawn the browser: a foreground title cannot hand its screen
 * to an elfldr payload, which is exactly why the browser must become the
 * native eboot.bin itself.
 */
#include <stdint.h>
#include <string.h>

int sceKernelDebugOutText(int channel, const char *text);

struct notification_request {
    uint8_t reserved[45];
    char message[3075];
};
int sceKernelSendNotificationRequest(uint32_t device, void *request,
                                     size_t size, int blocking);

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    static const char line[] =
        "[prosperobrowser-native] PRSB00001 loader stub: the browser is not "
        "yet relinked as a native title; this tile is a placeholder.\n";
    sceKernelDebugOutText(0, line);

    static struct notification_request req;
    memset(&req, 0, sizeof(req));
    strncpy(req.message,
            "ProsperoBrowser: native title placeholder. Launch the browser "
            "through the websrv tile for now (see docs/native-title.md).",
            sizeof(req.message) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);

    return 0;
}
