/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "hwcomposer"
#include <hardware/hardware.h>

#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/resource.h>

#include <EGL/egl.h>

#define HWC_REMOVE_DEPRECATED_VERSIONS 1

#include <cutils/compiler.h>
#include <cutils/log.h>
#include <cutils/atomic.h>
#include <cutils/properties.h>

#include <utils/String8.h>
#include <hardware/hwcomposer.h>

#include <EGL/egl.h>
#include <utils/Vector.h>
#include <utils/Timers.h>
// for private_handle_t
#include <gralloc_priv.h>

#include <Amavutils.h>
#include <system/graphics.h>
#ifndef LOGD
#define LOGD ALOGD
#endif

extern "C" int clock_nanosleep(clockid_t clock_id, int flags,
                           const struct timespec *request,
                           struct timespec *remain);
/*****************************************************************************/


struct hwc_context_1_t {
    hwc_composer_device_1_t device;
    /* our private state goes below here */
		hwc_layer_1_t const* saved_layer;
    unsigned saved_transform;
    int saved_left;
    int saved_top;
    int saved_right;
    int saved_bottom;
	int vsync_enable;
    const hwc_procs_t       *procs;
    pthread_t               vsync_thread;
};

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device);

static struct hw_module_methods_t hwc_module_methods = {
    open: hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: HWC_HARDWARE_MODULE_ID,
        name: "hwcomposer module",
        author: "Amlogic",
        methods: &hwc_module_methods,
        dso : NULL,
        reserved : {0},
    }
};

static pthread_cond_t hwc_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t hwc_mutex = PTHREAD_MUTEX_INITIALIZER;


/*****************************************************************************/

int video_on_vpp2_enabled(void)
{
    int ret = 0;
    
    // ro.vout.dualdisplay4
    char val[32];
    memset(val, 0, sizeof(val));
    if (property_get("ro.vout.dualdisplay4", val, "false")
        && strcmp(val, "true") == 0) {       
        ret = 1;
    }

    return ret;
}

static void hwc_overlay_compose(hwc_composer_device_1_t *dev, hwc_layer_1_t const* l) {
    int angle;
    struct hwc_context_1_t* ctx = (struct hwc_context_1_t*)dev;

    static char last_val[32] = "0";
    int vpp_changed = 0;
    char buf[40] = {0};
    if (video_on_vpp2_enabled()) {
        char val[32];
        memset(val, 0, sizeof(val));    
        if (amsysfs_get_sysfs_str("/sys/module/amvideo/parameters/cur_dev_idx", val, sizeof(val)) == 0) {        
            if ((strncmp(val, last_val, 1) != 0)) {
                strcpy(last_val, val);
                vpp_changed = 1;
            }
        }
    }

    if ((ctx->saved_layer == l) &&
        (ctx->saved_transform == l->transform) &&
        (ctx->saved_left == l->displayFrame.left) &&
        (ctx->saved_top == l->displayFrame.top) &&
        (ctx->saved_right == l->displayFrame.right) &&
        (ctx->saved_bottom == l->displayFrame.bottom) &&
        !vpp_changed) {
        return;
    }

    switch (l->transform) {
        case 0:
            angle = 0;
            break;
        case HAL_TRANSFORM_ROT_90:
            angle = 90;
            break;
        case HAL_TRANSFORM_ROT_180:
            angle = 180;
            break;
        case HAL_TRANSFORM_ROT_270:
            angle = 270;
            break;
        default:
            return;
    }
    
    property_get("media.p2pplay.enable", buf, "false");

    if (!strncmp(buf, "true", 4)){
    
        memset(buf, 0, 40);
        property_get("const.window.w", buf, "1280") ;
        int disp_w = atoi(buf)>0? atoi(buf): disp_w; 
        memset(buf, 0, 40);
        property_get("const.window.h", buf, "720") ;
        int disp_h = atoi(buf)>0? atoi(buf): disp_h;
        int x = l->displayFrame.left;
        int y = l->displayFrame.top;
        int w = l->displayFrame.right - l->displayFrame.left + 1;
        int h = l->displayFrame.bottom - l->displayFrame.top + 1;
        if(w > disp_w)
            w = disp_w;
        if(h > disp_h)
            h = disp_h;
        amvideo_utils_set_virtual_position(x, y, w, h, angle);
    }
    else
    {
        amvideo_utils_set_virtual_position(l->displayFrame.left,
                                           l->displayFrame.top,
                                           l->displayFrame.right - l->displayFrame.left + 1,
                                           l->displayFrame.bottom - l->displayFrame.top + 1,
                                           angle);

    }
    /* the screen mode from Android framework should always be set to normal mode
     * to match the relationship between the UI and video overlay window position.
     */
    amvideo_utils_set_screen_mode(0);

    ctx->saved_layer = l;
    ctx->saved_transform = l->transform;
    ctx->saved_left = l->displayFrame.left;
    ctx->saved_top = l->displayFrame.top;
    ctx->saved_right = l->displayFrame.right;
    ctx->saved_bottom = l->displayFrame.bottom;
}

/*static void dump_layer(hwc_layer_t const* l) {
    LOGD("\ttype=%d, flags=%08x, handle=%p, tr=%02x, blend=%04x, {%d,%d,%d,%d}, {%d,%d,%d,%d}",
            l->compositionType, l->flags, l->handle, l->transform, l->blending,
            l->sourceCrop.left,
            l->sourceCrop.top,
            l->sourceCrop.right,
            l->sourceCrop.bottom,
            l->displayFrame.left,
            l->displayFrame.top,
            l->displayFrame.right,
            l->displayFrame.bottom);
}*/

static int hwc_blank(struct hwc_composer_device_1* dev,
                     int disp,
                     int blank)
{
    return 0;
}

static int hwc_eventControl(struct hwc_composer_device_1* dev,
                            int disp,
                            int event,
                            int enabled)
{
    struct hwc_context_1_t* ctx = (struct hwc_context_1_t *)dev;
    switch (event) 
    {
        case HWC_EVENT_VSYNC:
            ctx->vsync_enable =enabled;
            pthread_mutex_lock(&hwc_mutex);
            pthread_cond_signal(&hwc_cond);
            pthread_mutex_unlock(&hwc_mutex);
            return 0;    
    }    
	return -EINVAL;
}

static int hwc_prepare(struct hwc_composer_device_1 *dev,
                       size_t numDisplays,
                       hwc_display_contents_1_t** displays)
{
    return 0;
}

static int hwc_set(struct hwc_composer_device_1 *dev,
                   size_t numDisplays,
                   hwc_display_contents_1_t** displays)
{
    // On version 1.0(HWC_DEVICE_API_VERSION_1_0), the OpenGL ES target surface is communicated
    // by the (dpy, sur) fields and we are guaranteed to have only
    // a single display.
    if (numDisplays != 1) {
        return 0;
    }

    hwc_display_contents_1_t *list = displays[0];
    for (size_t i=0 ; i<list->numHwLayers ; i++) {
        hwc_layer_1_t* l = &list->hwLayers[i];
        if (l->handle) {
            private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(l->handle);
            if (hnd->flags & private_handle_t::PRIV_FLAGS_VIDEO_OVERLAY) {
                hwc_overlay_compose(dev, l);
            }
        }
    }


    EGLBoolean success = eglSwapBuffers(displays[0]->dpy, displays[0]->sur);
    if (!success) {
        return HWC_EGL_ERROR;
    }
    return 0;
}

static int hwc_device_close(struct hw_device_t *dev)
{
    struct hwc_context_1_t* ctx = (struct hwc_context_1_t*)dev;
    if (ctx) {
        free(ctx);
    }
    return 0;
}



static void *hwc_vsync_thread(void *data)
{
    struct hwc_context_1_t* ctx = (struct hwc_context_1_t*)data;
    nsecs_t nextFakeVSync = 0;

    setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY-1);
	sleep(2);

    while (true) {
        pthread_mutex_lock(&hwc_mutex);
        while(ctx->vsync_enable==false)
        {
            pthread_cond_wait(&hwc_cond, &hwc_mutex);
        }
        pthread_mutex_unlock(&hwc_mutex);

        const nsecs_t period = 20000000; //50Hz
        const nsecs_t now = systemTime(CLOCK_MONOTONIC);
        nsecs_t next_vsync = nextFakeVSync;
        nsecs_t sleep = next_vsync - now;
        if (sleep < 0) {
            // we missed, find where the next vsync should be
            sleep = (period - ((now - next_vsync) % period));
            next_vsync = now + sleep;
        }
        nextFakeVSync = next_vsync + period;

        struct timespec spec;
        spec.tv_sec  = next_vsync / 1000000000;
        spec.tv_nsec = next_vsync % 1000000000;

        int err;
        do {
            err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &spec, NULL);
        } while (err<0 && errno == EINTR);

        if (err == 0) {
            if (ctx->procs) {
                ctx->procs->vsync(ctx->procs, 0, next_vsync);
            }
        }
    }

    return NULL;
}



static void hwc_registerProcs(hwc_composer_device_1_t *dev,
            hwc_procs_t const* procs)
{
    struct hwc_context_1_t* ctx = (struct hwc_context_1_t*)dev;
    if (ctx) {
        ctx->procs = procs;
    }
}

/*****************************************************************************/

static int hwc_device_open(const struct hw_module_t* module, const char* name,
        struct hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, HWC_HARDWARE_COMPOSER)) {
        struct hwc_context_1_t *dev;
        dev = (hwc_context_1_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = HWC_DEVICE_API_VERSION_1_0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = hwc_device_close;

        dev->device.blank = hwc_blank;
        dev->device.eventControl = hwc_eventControl;
        dev->device.registerProcs = hwc_registerProcs;
        dev->device.prepare = hwc_prepare;
        dev->device.set = hwc_set;
        dev->vsync_enable = false;
        *device = &dev->device.common;
        status = 0;
        
        status = pthread_create(&dev->vsync_thread, NULL, hwc_vsync_thread, dev);
        if (status) {
            ALOGE("failed to start vsync thread: %s", strerror(status));
        }
    }
    return status;
}
