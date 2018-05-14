#include <citro2d.h>
#include <stdlib.h>
#include <malloc.h>
#include <curl/curl.h>
#include "quirc/quirc.h"

// Type to store all our QR thread/data informations
typedef struct {
    u16*          camera_buffer;
    Handle        mutex;
    volatile bool finished;
    Handle        cancel;
    bool          capturing;
    struct quirc* context;
    // C2D vars
    C3D_Tex*      tex;
    C2D_Image     image;
    C2D_Text      state;
} qr_data;

// increase the stack in order to allow quirc to decode large qrs
int __stacksize__ = 64 * 1024;

// c3d screen targets
static C3D_RenderTarget* top;
static C3D_RenderTarget* bottom;

// c2d text vars
static C2D_TextBuf staticBuf, dynamicBuf;
static C2D_Text c2dTitle, c2dInstructions, c2dStateReady, c2dStateDownloading, c2dStateInstalling, c2dResult;
static Result result;

static void qrScanner(void);
static void qrHandler(qr_data* data);
static void qrExit(qr_data* data);
static void camThread(void* arg);
static void uiThread(void* arg);
static void download(const uint8_t* url);
static size_t handle_data(char* ptr, size_t size, size_t nmemb, void* userdata);
static void deletePrevious(u64 titleid);
static void installCia(const char* path);

static void rectangle(int x, int y, int w, int h, u32 c)
{
    C2D_DrawTriangle(x, y, c, x, y+h, c, x+w, y, c, 0.5f);
    C2D_DrawTriangle(x+w, y, c, x, y+h, c, x+w, y+h, c, 0.5f);
}

int main()
{
    // store the old time limit to reset when the app ends
    u32 old_time_limit;

    APT_GetAppCpuTimeLimit(&old_time_limit);
    APT_SetAppCpuTimeLimit(30);

    amInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // parse c2d text
    staticBuf = C2D_TextBufNew(256);
    dynamicBuf = C2D_TextBufNew(32);
    C2D_TextParse(&c2dTitle, staticBuf, "QRaken - TLSv1.2 compatible QR code scanner");
    C2D_TextParse(&c2dInstructions, staticBuf, "Press START to exit.");
    C2D_TextParse(&c2dStateReady, staticBuf, "State: READY");
    C2D_TextParse(&c2dStateDownloading, staticBuf, "State: DOWNLOADING");
    C2D_TextParse(&c2dStateInstalling, staticBuf, "State: INSTALLING");
    C2D_TextOptimize(&c2dTitle);
    C2D_TextOptimize(&c2dInstructions);
    C2D_TextOptimize(&c2dStateReady);
    C2D_TextOptimize(&c2dStateDownloading); 
    C2D_TextOptimize(&c2dStateInstalling);

    // jump to main loop
    qrScanner();

    // free environment
    C2D_TextBufDelete(dynamicBuf);
    C2D_TextBufDelete(staticBuf);

    // close services
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    amExit();

    if (old_time_limit != UINT32_MAX)
    {
        APT_SetAppCpuTimeLimit(old_time_limit);
    }

    return 0;
}

static void qrScanner(void)
{
    // init qr_data struct variables
    qr_data* data = malloc(sizeof(qr_data));
    data->capturing = false;
    data->finished = false;
    data->context = quirc_new();
    quirc_resize(data->context, 400, 240);
    data->camera_buffer = calloc(1, 400 * 240 * sizeof(u16));
    data->state = c2dStateReady;
    data->tex = (C3D_Tex*)malloc(sizeof(C3D_Tex));
    static const Tex3DS_SubTexture subt3x = { 512, 256, 0.0f, 1.0f, 1.0f, 0.0f };
    data->image = (C2D_Image){ data->tex, &subt3x };
    C3D_TexInit(data->image.tex, 512, 256, GPU_RGB565);
    C3D_TexSetFilter(data->image.tex, GPU_LINEAR, GPU_LINEAR);

    threadCreate(uiThread, data, 0x10000, 0x1A, 1, true);
    while (!data->finished)
    {
        qrHandler(data);
    }
}

// main loop
static void qrHandler(qr_data *data)
{
    hidScanInput();
    if (hidKeysDown() & KEY_START)
    {
        qrExit(data);
        return;
    }

    if (!data->capturing)
    {
        // create cam thread
        data->mutex = 0;
        data->cancel = 0;
        svcCreateEvent(&data->cancel, RESET_STICKY);
        svcCreateMutex(&data->mutex, false);
        if (threadCreate(camThread, data, 0x10000, 0x1A, 1, true) != NULL)
        {
            data->capturing = true;
        }
        else
        {
            qrExit(data);
            return;
        }
    }

    if (data->finished)
    {
        qrExit(data);
        return;
    }

    int w, h;
    u8* image = (u8*)quirc_begin(data->context, &w, &h);
    svcWaitSynchronization(data->mutex, U64_MAX);
    for (ssize_t x = 0; x < w; x++)
    {
        for (ssize_t y = 0; y < h; y++)
        {
            u16 px = data->camera_buffer[y * 400 + x];
            image[y * w + x] = (u8)(((((px >> 11) & 0x1F) << 3) + (((px >> 5) & 0x3F) << 2) + ((px & 0x1F) << 3)) / 3);
        }
    }
    svcReleaseMutex(data->mutex);
    quirc_end(data->context);
    if (quirc_count(data->context) > 0)
    {
        struct quirc_code code;
        struct quirc_data scan_data;
        quirc_extract(data->context, 0, &code);  
        if (!quirc_decode(&code, &scan_data))
        {
            char url[scan_data.payload_len];
            memcpy(url, scan_data.payload, scan_data.payload_len);
            if (strstr(strlwr(url), ".cia") != NULL)
            {
                data->state = c2dStateDownloading;
                download(scan_data.payload);
                // install from file
                if (R_SUCCEEDED(result))
                {
                    data->state = c2dStateInstalling;
                    installCia("/tmp.cia");
                    remove("/tmp.cia");
                }
            }
            data->state = c2dStateReady;
        }
    }
}

static void qrExit(qr_data *data)
{
    svcSignalEvent(data->cancel);
    while (!data->finished)
    {
        svcSleepThread(1000000);
    }
    data->capturing = false;
    free(data->camera_buffer);
    free(data->tex);
    quirc_destroy(data->context);
    free(data);
}

static void camThread(void *arg) 
{
    qr_data* data = (qr_data*) arg;
    Handle events[3] = {0};
    events[0] = data->cancel;
    u32 transferUnit;

    u16 *buffer = malloc(400 * 240 * sizeof(u16));
    camInit();
    CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
    CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
    CAMU_SetNoiseFilter(SELECT_OUT1, true);
    CAMU_SetAutoExposure(SELECT_OUT1, true);
    CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    CAMU_Activate(SELECT_OUT1);
    CAMU_GetBufferErrorInterruptEvent(&events[2], PORT_CAM1);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_GetMaxBytes(&transferUnit, 400, 240);
    CAMU_SetTransferBytes(PORT_CAM1, transferUnit, 400, 240);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_SetReceiving(&events[1], buffer, PORT_CAM1, 400 * 240 * sizeof(u16), (s16) transferUnit);
    CAMU_StartCapture(PORT_CAM1);
    bool cancel = false;
    while (!cancel) 
    {
        s32 index = 0;
        svcWaitSynchronizationN(&index, events, 3, false, U64_MAX);
        switch (index)
        {
            case 0:
                cancel = true;
                break;
            case 1:
                svcCloseHandle(events[1]);
                events[1] = 0;
                svcWaitSynchronization(data->mutex, U64_MAX);
                memcpy(data->camera_buffer, buffer, 400 * 240 * sizeof(u16));
                GSPGPU_FlushDataCache(data->camera_buffer, 400 * 240 * sizeof(u16));
                svcReleaseMutex(data->mutex);
                CAMU_SetReceiving(&events[1], buffer, PORT_CAM1, 400 * 240 * sizeof(u16), transferUnit);
                break;
            case 2:
                svcCloseHandle(events[1]);
                events[1] = 0;
                CAMU_ClearBuffer(PORT_CAM1);
                CAMU_SetReceiving(&events[1], buffer, PORT_CAM1, 400 * 240 * sizeof(u16), transferUnit);
                CAMU_StartCapture(PORT_CAM1);
                break;
            default:
                break;
        }
    }

    CAMU_StopCapture(PORT_CAM1);

    bool busy = false;
    while (R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy)
    {
        svcSleepThread(1000000);
    }

    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    camExit();
    free(buffer);
    for (int i = 0; i < 3; i++)
    {
        if (events[i] != 0)
        {
            svcCloseHandle(events[i]);
            events[i] = 0;
        }
    }
    svcCloseHandle(data->mutex);
    data->finished = true;
}

static void uiThread(void* arg)
{
    qr_data* data = (qr_data*) arg;
    while (!data->finished)
    {
        static const float scale = 0.5f;
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        for (u32 x = 0; x < 400; x++)
        {
            for (u32 y = 0; y < 240; y++)
            {
                u32 dstPos = ((((y >> 3) * (512 >> 3) + (x >> 3)) << 6) + ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))) * 2;
                u32 srcPos = (y * 400 + x) * 2;
                memcpy(&((u8*)data->image.tex->data)[dstPos], &((u8*)data->camera_buffer)[srcPos], 2);
            }
        }

        C2D_TargetClear(bottom, !memcmp(&data->state, &c2dStateReady, sizeof(C2D_Text)) ? C2D_Color32(0x20, 0x20, 0x20, 0xFF) : C2D_Color32(0xF4, 0xC, 0x0, 0xFF));
        C2D_SceneBegin(top);
        C2D_DrawImageAt(data->image, 0.0f, 0.0f, 0.5f, NULL, 1.0f, 1.0f);
        
        C2D_SceneBegin(bottom);
        rectangle(0, 0, 320, 20, C2D_Color32(0x70, 0x70, 0x70, 0xFF));
        rectangle(0, 220, 320, 20, C2D_Color32(0x70, 0x70, 0x70, 0xFF));

        // state bars
        rectangle(4, 34, 312, 24, C2D_Color32f(0, 0, 0, 1));
        rectangle(4, 60, 312, 24, C2D_Color32f(0, 0, 0, 1));
        rectangle(6, 36, 308, 20, C2D_Color32f(1, 1, 1, 1));
        rectangle(6, 62, 308, 20, C2D_Color32f(1, 1, 1, 1));

        C2D_TextBufClear(dynamicBuf);
        static char buffer[32];
        snprintf(buffer, 32, "Result code: 0x%08lX", result);
        C2D_TextParse(&c2dResult, dynamicBuf, buffer);
        C2D_TextOptimize(&c2dResult);

        float w, h;
        C2D_TextGetDimensions(&c2dInstructions, scale, scale, &w, &h);
        C2D_DrawText(&c2dTitle, C2D_WithColor, 4, (20-h)/2, 0.5f, scale, scale, C2D_Color32f(1, 1, 1, 1));
        C2D_DrawText(&c2dInstructions, C2D_WithColor, (320-w)/2, 220 + (20-h)/2, 0.5f, scale, scale, C2D_Color32f(1, 1, 1, 1));

        C2D_DrawText(&data->state, C2D_WithColor, 8, 36 + (20-h)/2, 0.5f, scale, scale, C2D_Color32f(0, 0, 0, 1));
        C2D_DrawText(&c2dResult, C2D_WithColor, 8, 62 + (20-h)/2, 0.5f, scale, scale, C2D_Color32f(0, 0, 0, 1));
        C3D_FrameEnd(0);
    }
}

static char* result_buf = NULL;
static size_t result_sz = 0;
static size_t result_written = 0;

static void download(const uint8_t* url)
{
    void *socubuf = memalign(0x1000, 0x100000);
    if (!socubuf)
    {
        result = -1;
        return;
    }

    if (R_FAILED(result = socInit(socubuf, 0x100000)))
    {
        return;
    }

    CURL *hnd = curl_easy_init();
    curl_easy_setopt(hnd, CURLOPT_BUFFERSIZE, 102400L);
    curl_easy_setopt(hnd, CURLOPT_URL, url);
    curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(hnd, CURLOPT_USERAGENT, "QRaken-curl/7.58.0");
    curl_easy_setopt(hnd, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50L);
    curl_easy_setopt(hnd, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, handle_data);
    curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(hnd, CURLOPT_VERBOSE, 1L);
    CURLcode cres = curl_easy_perform(hnd);
    
    // cleanup
    curl_easy_cleanup(hnd);
    socExit();
    free(socubuf);
    result_sz = 0;

    if (cres == CURLE_OK)
    {
        // write to disk
        FILE* file = fopen("/tmp.cia", "wb");
        fwrite(result_buf, result_written, 1, file);
        fclose(file);
    }

    free(result_buf);
    result_buf = NULL;
    result_written = 0;
    result = cres == CURLE_OK ? 0 : (Result)cres;
}

// following code is from 
// https://github.com/angelsl/libctrfgh/blob/master/curl_test/src/main.c
static size_t handle_data(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    (void) userdata;
    const size_t bsz = size*nmemb;

    if (result_sz == 0 || !result_buf)
    {
        result_sz = 0x1000;
        result_buf = malloc(result_sz);
    }

    bool need_realloc = false;
    while (result_written + bsz > result_sz) 
    {
        result_sz <<= 1;
        need_realloc = true;
    }

    if (need_realloc)
    {
        char *new_buf = realloc(result_buf, result_sz);
        if (!new_buf)
        {
            return 0;
        }
        result_buf = new_buf;
    }

    if (!result_buf)
    {
        return 0;
    }

    memcpy(result_buf + result_written, ptr, bsz);
    result_written += bsz;
    return bsz;
}

// the following code is from
// https://github.com/LiquidFenrir/MultiUpdater/blob/rewrite/source/cia.c
static void deletePrevious(u64 titleid)
{
    u32 titles_amount = 0;
    if (R_FAILED(result = AM_GetTitleCount(MEDIATYPE_SD, &titles_amount)))
    {
        return;
    }
    
    u32 read_titles = 0;
    u64* titleIDs = malloc(titles_amount * sizeof(u64));
    if (R_FAILED(result = AM_GetTitleList(&read_titles, MEDIATYPE_SD, titles_amount, titleIDs)))
    {
        free(titleIDs);
        return;
    }
    
    for (u32 i = 0; i < read_titles; i++)
    {
        if (titleIDs[i] == titleid)
        {
            result = AM_DeleteAppTitle(MEDIATYPE_SD, titleid);
            break;
        }
    }
    
    free(titleIDs);
}

static void installCia(const char* ciaPath)
{
    u64 size = 0;
    u32 bytes;
    Handle ciaHandle, fileHandle;
    AM_TitleEntry info;
    
    result = FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, ciaPath), FS_OPEN_READ, 0);
    if (R_FAILED(result))
    {
        return;
    }
    
    if (R_FAILED(result = AM_GetCiaFileInfo(MEDIATYPE_SD, &info, fileHandle)))
    {
        return;
    }
    
    deletePrevious(info.titleID);
    if (R_FAILED(result))
    {
        return;
    }
    
    if (R_FAILED(result = FSFILE_GetSize(fileHandle, &size)))
    {
        return;
    }

    if (R_FAILED(result = AM_StartCiaInstall(MEDIATYPE_SD, &ciaHandle)))
    {
        return;
    }
    
    u32 toRead = 0x1000;
    u8* cia_buffer = malloc(toRead);
    for (u64 startSize = size; size != 0; size -= toRead)
    {
        if (size < toRead) toRead = size;
        FSFILE_Read(fileHandle, &bytes, startSize-size, cia_buffer, toRead);
        FSFILE_Write(ciaHandle, &bytes, startSize-size, cia_buffer, toRead, 0);
    }
    free(cia_buffer);

    if (R_FAILED(result = AM_FinishCiaInstall(ciaHandle)))
    {
        return;
    }

    result = FSFILE_Close(fileHandle);
}