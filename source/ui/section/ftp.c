#include <arpa/inet.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include "section.h"
#include "../mainmenu.h"
#include "../ui.h"
#include "../list.h"
#include "../error.h"
#include "../info.h"
#include "../prompt.h"
#include "../../core/screen.h"
#include "../../ftpd/ftp.h"
#include "../../ftpd/console.h"

typedef struct{
    bool ftpInitialised;
}ftp_data;

static void ftp_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    u32 logoWidth;
    u32 logoHeight;
    screen_get_texture_size(&logoWidth, &logoHeight, TEXTURE_FTP);

    float logoX = x1 + (x2 - x1 - logoWidth) / 2;
    float logoY = y1 + (y2 - y1 - logoHeight) / 2;
    screen_draw_texture(TEXTURE_FTP, logoX, logoY, logoWidth, logoHeight);

    char verString[64];
    snprintf(verString, 64, "Super ftpd II Turbo (Ver. %s)", "2.2");

    float verWidth;
    float verHeight;
    screen_get_string_size(&verWidth, &verHeight, verString, 0.5f, 0.5f);

    float verX = x1 + (x2 - x1 - verWidth) / 2;
    float verY = logoY + logoHeight + (y2 - (logoY + logoHeight) - verHeight) / 2;
    screen_draw_string(verString, verX, verY, 0.5f, 0.5f, COLOR_TEXT, false);
}
      
static void ftp_wait_update(ui_view* view, void* data, float* progress, char* text) {
    ftp_data*  ftpdata= (ftp_data*)data;

    if(hidKeysDown() & KEY_B) {
        sdmcWriteSafe(true);
        if(ftpdata->ftpInitialised)
            ftp_exit();
        ui_pop();
        info_destroy(view);
        free(ftpdata);
        return;
    }
    if(ftpdata->ftpInitialised) {
        struct in_addr addr = {(in_addr_t) gethostid()};
        snprintf(text, PROGRESS_TEXT_MAX, "Ready!\nIP: %s\nPort: 5000", inet_ntoa(addr));
        /* ftp loop */
        if(ftp_loop() == LOOP_EXIT) {
            /* done with ftp */
            ftp_exit();
            ftpdata->ftpInitialised = false;
        }
    }
    else {
        u32    wifi = 0;
        Result ret = ACU_GetWifiStatus(&wifi);
        if(ret == 0)
            ftpdata->ftpInitialised = (ftp_init() == 0);

        snprintf(text, PROGRESS_TEXT_MAX, "Waiting for wifi...\n");
    }
}

void ftp_open() {
    sdmcWriteSafe(false);
    /* initialize ftp subsystem */
    ftp_data* data = (ftp_data*) calloc(1, sizeof(ftp_data));
    data->ftpInitialised = false;
    info_display("FTP", "B: Return", false, data, ftp_wait_update, ftp_draw_top);
}


