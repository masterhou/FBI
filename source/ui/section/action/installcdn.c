#include <malloc.h>
#include <stdio.h>

#include <3ds.h>

#include "action.h"
#include "../task/task.h"
#include "../../error.h"
#include "../../info.h"
#include "../../list.h"
#include "../../prompt.h"
#include "../../ui.h"
#include "../../../core/linkedlist.h"
#include "../../../core/screen.h"
#include "../../../core/util.h"

#define CONTENTS_MAX 256

typedef struct {
    ticket_info* ticket;
    volatile bool* done;
    bool finishedPrompt;

    u32 contentCount;
    u16 contentIndices[CONTENTS_MAX];
    u32 contentIds[CONTENTS_MAX];

    u32 responseCode;

    data_op_data installInfo;
} install_cdn_data;

static Result action_install_cdn_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

static Result action_install_cdn_make_dst_directory(void* data, u32 index) {
    return 0;
}

static Result action_install_cdn_open_src(void* data, u32 index, u32* handle) {
    install_cdn_data* installData = (install_cdn_data*) data;

    Result res = 0;

    httpcContext* context = (httpcContext*) calloc(1, sizeof(httpcContext));
    if(context != NULL) {
        char url[256];
        if(index == 0) {
            snprintf(url, 256, "http://ccs.cdn.c.shop.nintendowifi.net/ccs/download/%016llX/tmd", installData->ticket->titleId);
        } else {
            snprintf(url, 256, "http://ccs.cdn.c.shop.nintendowifi.net/ccs/download/%016llX/%08lX", installData->ticket->titleId, installData->contentIds[index - 1]);
        }

        if(R_SUCCEEDED(res = httpcOpenContext(context, HTTPC_METHOD_GET, url, 1))) {
            httpcSetSSLOpt(context, SSLCOPT_DisableVerify);
            if(R_SUCCEEDED(res = httpcBeginRequest(context)) && R_SUCCEEDED(res = httpcGetResponseStatusCode(context, &installData->responseCode, 0))) {
                if(installData->responseCode == 200) {
                    *handle = (u32) context;
                } else {
                    res = R_FBI_HTTP_RESPONSE_CODE;
                }
            }

            if(R_FAILED(res)) {
                httpcCloseContext(context);
            }
        }

        if(R_FAILED(res)) {
            free(context);
        }
    } else {
        res = R_FBI_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_install_cdn_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return httpcCloseContext((httpcContext*) handle);
}

static Result action_install_cdn_get_src_size(void* data, u32 handle, u64* size) {
    u32 downloadSize = 0;
    Result res = httpcGetDownloadSizeState((httpcContext*) handle, NULL, &downloadSize);

    *size = downloadSize;
    return res;
}

static Result action_install_cdn_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    Result res = httpcDownloadData((httpcContext*) handle, buffer, size, bytesRead);
    return res != HTTPC_RESULTCODE_DOWNLOADPENDING ? res : 0;
}

static Result action_install_cdn_open_dst(void* data, u32 index, void* initialReadBlock, u32* handle) {
    install_cdn_data* installData = (install_cdn_data*) data;

    if(index == 0) {
        installData->contentCount = util_get_tmd_content_count((u8*) initialReadBlock);
        if(installData->contentCount > CONTENTS_MAX) {
            return R_FBI_OUT_OF_RANGE;
        }

        for(u32 i = 0; i < installData->contentCount; i++) {
            u8* contentChunk = util_get_tmd_content_chunk((u8*) initialReadBlock, i);

            installData->contentIds[i] = __builtin_bswap32(*(u32*) &contentChunk[0x00]);
            installData->contentIndices[i] = __builtin_bswap16(*(u16*) &contentChunk[0x04]);
        }

        installData->installInfo.total += installData->contentCount;

        return AM_InstallTmdBegin(handle);
    } else {
        return AM_InstallContentBegin(handle, installData->contentIndices[index - 1]);
    }
}

static Result action_install_cdn_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    if(succeeded) {
        if(index == 0) {
            return AM_InstallTmdFinish(handle, true);
        } else {
            return AM_InstallContentFinish(handle);
        }
    } else {
        if(index == 0) {
            return AM_InstallTmdAbort(handle);
        } else {
            return AM_InstallContentCancel(handle);
        }
    }
}

static Result action_install_cdn_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

bool action_install_cdn_error(void* data, u32 index, Result res) {
    install_cdn_data* installData = (install_cdn_data*) data;

    if(res == R_FBI_CANCELLED) {
        prompt_display("Failure", "Install cancelled.", COLOR_TEXT, false, installData->ticket, NULL, ui_draw_ticket_info, NULL);
    } else if(res == R_FBI_HTTP_RESPONSE_CODE) {
        error_display(NULL, installData->ticket, ui_draw_ticket_info, "Failed to install CDN title.\nHTTP server returned response code %d", installData->responseCode);
    } else {
        error_display_res(NULL, installData->ticket, ui_draw_ticket_info, res, "Failed to install CDN title.");
    }

    return false;
}

static void action_install_cdn_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ui_draw_ticket_info(view, ((install_cdn_data*) data)->ticket, x1, y1, x2, y2);
}

static void action_install_cdn_free_data(install_cdn_data* data) {
    if(data->done != NULL) {
        *data->done = true;
    }

    free(data);
}

static void action_install_cdn_update(ui_view* view, void* data, float* progress, char* text) {
    install_cdn_data* installData = (install_cdn_data*) data;

    if(installData->installInfo.finished) {
        ui_pop();
        info_destroy(view);

        Result res = 0;

        if(R_SUCCEEDED(installData->installInfo.result)) {
            if(R_SUCCEEDED(res = AM_InstallTitleFinish())
               && R_SUCCEEDED(res = AM_CommitImportTitles(((installData->ticket->titleId >> 32) & 0x8010) != 0 ? MEDIATYPE_NAND : MEDIATYPE_SD, 1, false, &installData->ticket->titleId))) {
                util_import_seed(installData->ticket->titleId);

                if(installData->ticket->titleId == 0x0004013800000002 || installData->ticket->titleId == 0x0004013820000002) {
                    res = AM_InstallFirm(installData->ticket->titleId);
                }
            }
        }

        if(R_SUCCEEDED(installData->installInfo.result) && R_SUCCEEDED(res)) {
            if(installData->finishedPrompt) {
                prompt_display("Success", "Install finished.", COLOR_TEXT, false, installData->ticket, NULL, ui_draw_ticket_info, NULL);
            }
        } else {
            AM_InstallTitleAbort();

            if(R_FAILED(res)) {
                error_display_res(NULL, installData->ticket, ui_draw_ticket_info, res, "Failed to install CDN title.");
            }
        }

        action_install_cdn_free_data(installData);

        return;
    }

    if((hidKeysDown() & KEY_B) && !installData->installInfo.finished) {
        svcSignalEvent(installData->installInfo.cancelEvent);
    }

    *progress = installData->installInfo.currTotal != 0 ? (float) ((double) installData->installInfo.currProcessed / (double) installData->installInfo.currTotal) : 0;
    snprintf(text, PROGRESS_TEXT_MAX, "%lu / %lu\n%.2f MiB / %.2f MiB", installData->installInfo.processed, installData->installInfo.total, installData->installInfo.currProcessed / 1024.0 / 1024.0, installData->installInfo.currTotal / 1024.0 / 1024.0);
}

void action_install_cdn_noprompt(volatile bool* done, ticket_info* info, bool finishedPrompt) {
    install_cdn_data* data = (install_cdn_data*) calloc(1, sizeof(install_cdn_data));
    if(data == NULL) {
        error_display(NULL, NULL, NULL, "Failed to allocate install CDN data.");

        return;
    }

    data->ticket = info;
    data->done = done;
    data->finishedPrompt = finishedPrompt;

    data->responseCode = 0;

    data->installInfo.data = data;

    data->installInfo.op = DATAOP_COPY;

    data->installInfo.copyEmpty = false;

    data->installInfo.total = 1;

    data->installInfo.isSrcDirectory = action_install_cdn_is_src_directory;
    data->installInfo.makeDstDirectory = action_install_cdn_make_dst_directory;

    data->installInfo.openSrc = action_install_cdn_open_src;
    data->installInfo.closeSrc = action_install_cdn_close_src;
    data->installInfo.getSrcSize = action_install_cdn_get_src_size;
    data->installInfo.readSrc = action_install_cdn_read_src;

    data->installInfo.openDst = action_install_cdn_open_dst;
    data->installInfo.closeDst = action_install_cdn_close_dst;
    data->installInfo.writeDst = action_install_cdn_write_dst;

    data->installInfo.error = action_install_cdn_error;

    data->installInfo.finished = true;

    Result res = 0;

    u8 n3ds = false;
    if(R_FAILED(APT_CheckNew3DS(&n3ds)) || n3ds || ((data->ticket->titleId >> 28) & 0xF) != 2) {
        FS_MediaType dest = ((data->ticket->titleId >> 32) & 0x8010) != 0 ? MEDIATYPE_NAND : MEDIATYPE_SD;

        AM_DeleteTitle(dest, data->ticket->titleId);
        if(dest == MEDIATYPE_SD) {
            AM_QueryAvailableExternalTitleDatabase(NULL);
        }

        if(R_SUCCEEDED(res = AM_InstallTitleBegin(dest, data->ticket->titleId, false))) {
            if(R_SUCCEEDED(res = task_data_op(&data->installInfo))) {
                info_display("Installing CDN Title", "Press B to cancel.", true, data, action_install_cdn_update, action_install_cdn_draw_top);
            } else {
                AM_InstallTitleAbort();
            }
        }
    } else {
        res = R_FBI_WRONG_SYSTEM;
    }

    if(R_FAILED(res)) {
        error_display_res(NULL, data->ticket, ui_draw_ticket_info, res, "Failed to initiate CDN title installation.");

        action_install_cdn_free_data(data);
    }
}

static void action_install_cdn_onresponse(ui_view* view, void* data, bool response) {
    ticket_info* info = (ticket_info*) data;

    if(response) {
        action_install_cdn_noprompt(NULL, info, true);
    }
}

void action_install_cdn(linked_list* items, list_item* selected) {
    prompt_display("Confirmation", "Install the selected title from the CDN?", COLOR_TEXT, true, selected->data, NULL, ui_draw_ticket_info, action_install_cdn_onresponse);
}