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

typedef struct{
    bool initialised;
    bool finished;
    Handle cancelEvent;
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

static void ftp_thread(void* arg) {
    ftp_data* data = (ftp_data*) arg;
    sdmcWriteSafe(false);
    while(svcWaitSynchronization(data->cancelEvent, 0) != 0){
        /* done with ftp */
        if(ftp_loop() == LOOP_EXIT) 
            break;
    }
    ftp_exit();
    sdmcWriteSafe(true);
    data->finished = true;
}      

static void ftp_wait_update(ui_view* view, void* data, float* progress, char* text) {
    ftp_data*  ftpdata= (ftp_data*)data;

    if(ftpdata->finished){
        svcCloseHandle(ftpdata->cancelEvent);
        free(ftpdata);

        ui_pop();
        info_destroy(view);

        return;       
    }

    if(hidKeysDown() & KEY_B) {
        svcSignalEvent(ftpdata->cancelEvent);
    }

    if(ftpdata->initialised) {
        struct in_addr addr = {(in_addr_t) gethostid()};
        snprintf(text, PROGRESS_TEXT_MAX, "Ready!\nIP: %s\nPort: 5000", inet_ntoa(addr));
    }
    else {
        u32    wifi = 0;
        Result ret = ACU_GetWifiStatus(&wifi);
        if(ret == 0)
            ftpdata->initialised = (ftp_init() == 0);
        if(ftpdata->initialised){
            if(threadCreate(ftp_thread, data, 0x10000, 0x18, 1, true) == NULL) {
                //error_display(NULL, NULL, NULL, "Failed to create ftp loop thread.");
                ftpdata->finished = true; 
                return;
            }
        }
        snprintf(text, PROGRESS_TEXT_MAX, "Waiting for wifi...\n");
    }
}

void ftp_open() {
    /* initialize ftp subsystem */
    ftp_data* data = (ftp_data*) calloc(1, sizeof(ftp_data));
    data->initialised = false;
    data->finished = false;

    Result eventRes = svcCreateEvent(&data->cancelEvent, 1);
    if(R_FAILED(eventRes)) {
        error_display_res(NULL, NULL, NULL, eventRes, "Failed to create ftp loop cancel event.");
        free(data);
        return;
    }

    info_display("FTP", "B: Return", false, data, ftp_wait_update, ftp_draw_top);
}


