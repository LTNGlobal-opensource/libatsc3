//
// Created by Jason Justman on 8/19/20.
//
//#define __JJUSTMAN_2020_12_24_SEA_533_SINGLE_PLP_TUNE_ONLY__

//jjustman-2021-09-01 - TODO: add in hook for:
//AT3DRV_RegisterL1DTimeInfoCallback(AT3_DEVICE hDev, AT3DRV_L1D_TIME_INFO_CB fnCallback, uint64_t ullUser);
//to investigate..rb-wakeup-level-atsc3
#include "LowaSISPHYAndroid.h"
LowaSISPHYAndroid* lowaSISPHYAndroid = nullptr;

mutex LowaSISPHYAndroid::CS_global_mutex;

/**
 * jjustman-2020-08-24 - LowaSIS helper defines

 */

#define ASSERT(cond,s) do { \
        if (!(cond)) { _LOWASIS_PHY_ANDROID_ERROR("%s: !! %s assert fail, line %d\n", __func__, s, __LINE__); \
            return -1; } \
        } while(0)

#define CHK_AR(ar,s) do { \
		if (ar) { _LOWASIS_PHY_ANDROID_ERROR("%s: !! %s, err %d, line %d\n", __func__, s, ar, __LINE__); \
			return -1; } \
		} while(0)
#define SHOW_AR(ar,s) do { \
		if (ar) { _LOWASIS_PHY_ANDROID_ERROR("%s: !! %s, err %d, line %d\n", __func__, s, ar, __LINE__); } \
		} while(0)


LowaSISPHYAndroid::LowaSISPHYAndroid(JNIEnv* env, jobject jni_instance) {
    this->env = env;
    this->jni_instance_globalRef = this->env->NewGlobalRef(jni_instance);
    this->setRxUdpPacketProcessCallback(atsc3_core_service_bridge_process_packet_from_plp_and_block);
    this->setRxLinkMappingTableProcessCallback(atsc3_phy_jni_bridge_notify_link_mapping_table);

    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->atsc3_phy_notify_plp_selection_change_set_callback(&LowaSISPHYAndroid::NotifyPlpSelectionChangeCallback, this);
    }

    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::LowaSISPHYAndroid - created with this: %p", this);
}

LowaSISPHYAndroid::~LowaSISPHYAndroid() {

    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::~LowaSISPHYAndroid - enter: deleting with this: %p", this);

    this->stop();

    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::~LowaSISPHYAndroid - exit: deleting with this: %p", this);
}

void LowaSISPHYAndroid::pinProducerThreadAsNeeded() {
    producerJniEnv = new Atsc3JniEnv(atsc3_ndk_phy_lowasis_static_loader_get_javaVM(), "LowaSISPHYAndroid::producerThread");
}

void LowaSISPHYAndroid::releasePinnedProducerThreadAsNeeded() {
    if(producerJniEnv) {
        delete producerJniEnv;
        producerJniEnv = nullptr;
    }
}

void LowaSISPHYAndroid::pinConsumerThreadAsNeeded() {
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::pinConsumerThreadAsNeeded: mJavaVM: %p, atsc3_ndk_application_bridge instance: %p", atsc3_ndk_phy_lowasis_static_loader_get_javaVM(), atsc3_ndk_application_bridge_get_instance());

    consumerJniEnv = new Atsc3JniEnv(atsc3_ndk_phy_lowasis_static_loader_get_javaVM(), "LowaSISPHYAndroid::consumerThread");
    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->pinConsumerThreadAsNeeded();
    }
}

void LowaSISPHYAndroid::releasePinnedConsumerThreadAsNeeded() {
    if(consumerJniEnv) {
        delete consumerJniEnv;
        consumerJniEnv = nullptr;
    }

    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->releasePinnedConsumerThreadAsNeeded();
    }
}

void LowaSISPHYAndroid::pinStatusThreadAsNeeded() {
    statusJniEnv = new Atsc3JniEnv(atsc3_ndk_phy_lowasis_static_loader_get_javaVM(), "LowaSISPHYAndroid::statusThread");

    if(atsc3_ndk_phy_bridge_get_instance()) {
        atsc3_ndk_phy_bridge_get_instance()->pinStatusThreadAsNeeded();
    }
}

void LowaSISPHYAndroid::releasePinnedStatusThreadAsNeeded() {
    if(statusJniEnv) {
        delete statusJniEnv;
        statusJniEnv = nullptr;
    }

    if(atsc3_ndk_phy_bridge_get_instance()) {
        atsc3_ndk_phy_bridge_get_instance()->releasePinnedStatusThreadAsNeeded();
    }
}

int LowaSISPHYAndroid::init()
{
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::init with this: %p", this);

    AT3RESULT ar;

    ar = AT3DRV_Init(AT3DRV_VER);
    if (!AT3OK(ar)) {
        _LOWASIS_PHY_ANDROID_ERROR("LowaSISPHYAndroid::init - AT3DRV_Init returned not ok! %d", ar);
        return ar;
    }

    S_AT3DRV_VER_INFO vinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    AT3DRV_GetVersionInfo(&vinfo);

    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::init - at3drv ver is: %s", vinfo.ver);

    init_completed = true;
    return 0;
}

const char* LowaSISPHYAndroid::getPhyFeVendorNameString(E_AT3_FEVENDOR phyFeVendor) {
    if(phyFeVendor == eAT3_FEVENDOR_LGDT3307_R850) {
        return "LG.3307";
    } else if(phyFeVendor == eAT3_FEVENDOR_CXD2878_ASCOT3) {
        return "SONY.2878";
    } else {
        return "(unknown)";
    }
}

//jjustman-2020-08-24 - copying from baseline impl
//
//int At3DrvIntf::Prepare(const char *strDevListInfo, int delim1, int delim2)
//{
//    // format example:  delim1 is colon, delim2 is comma
//    // "/dev/bus/usb/001/001:21,/dev/bus/usb/001/002:22"
//
//    AT3RESULT ar;
//    auto vsDevInfo = Split(strDevListInfo, (char)delim2);
//    if (!strDevListInfo || !*strDevListInfo || vsDevInfo.empty()) {
//        printf("empty device\n");
//        ar = AT3DRV_LDR_PolulateUsbDevices(nullptr, 0, nullptr);
//        CHK_AR(ar, "populate");
//        return 0;
//    }
//
//    int nNumDevs = (int)vsDevInfo.size();
//    vector<string> vsDevPath(nNumDevs);
//    vector<S_ETA_DEVICE> vEtaDevs(nNumDevs);
//
//    LogMsgF("prepare: %d devices", nNumDevs);
//    for (int i=0; i<nNumDevs; i++) {
//        printf("   -- %s\n", vsDevInfo[i].c_str());
//        auto vs = Split(vsDevInfo[i].c_str(), delim1);
//        if (vs.size() != 2) { // wrong form
//            printf("!! num word %d\n", (int)vs.size());
//            break;
//        }
//        int fd = atoi(vs[1].c_str());
//        if (fd == 0) { // probably wrong fd
//            printf("!! fd '%s'\n", vs[1].c_str());
//            break;
//        }
//        vsDevPath[i] = vs[0];
//        vEtaDevs[i].devfs = vsDevPath[i].c_str();
//        vEtaDevs[i].fd = fd;
//        printf("   (%d) %s, %d\n", i, vEtaDevs[i].devfs, vEtaDevs[i].fd);
//    }
//
//    int nDevAdded = 0;
//    ar = AT3DRV_LDR_PolulateUsbDevices(&vEtaDevs[0], nNumDevs, &nDevAdded);
//    CHK_AR(ar, "populate");
//    LogMsgF("populate: %d devices added", nDevAdded);
//
//    return 0;
//}
//
///*
// *
// */
//std::vector<AT3_DEV_KEY> At3DrvIntf::FindKeys(bool bPrebootDevice)
//{
//    E_AT3_DEV_FIND_FILTER filter = bPrebootDevice ? eDFF_CypressFx3 : eDFF_AtlasFx3;
//    std::vector<AT3_DEV_KEY> vKeys(128); // it can hold max 128 keys
//    int nKeys = 0;
//
//    AT3RESULT ar = AT3DRV_LDR_SearchDevicesByType(filter, &vKeys[0], 128, &nKeys);
//    if (!AT3OK(ar)) {
//        LogMsgF("search device failed %d", ar);
//        vKeys.clear();
//        return vKeys;
//    }
//    printf("%d keys found\n", nKeys);
//    vKeys.resize(nKeys);
//    return vKeys;
//}
//
///* Load atlas dongle firmware.
// * note: target device need to be populated before calling this api
// */
//int At3DrvIntf::FwLoad(AT3_DEV_KEY hKeyTarget)
//{
//    AT3RESULT ar;
//
//    LogMsgF("fwload (%llx)", (unsigned long long)hKeyTarget);
//
//    if (1) {
//        int i, nkey = 0;
//        AT3_DEV_KEY keys[32];
//        bool bDevFound = false;
//
//        ar = AT3DRV_LDR_SearchDevicesByType(eDFF_CypressFx3, keys, 32, &nkey);
//        CHK_AR(ar, "find cyfx3");
//
//        if (nkey <= 0) {
//            LogMsgF("no cyfx3");
//            return 0;
//        }
//        LogMsgF("==== all cypress fx3 devices (%d)", nkey);
//        for (i=0; i<nkey; i++) {
//            LogMsgF(" key[%d]: %lx", i, (unsigned long)keys[i]);
//            if (hKeyTarget == keys[i]) {
//                bDevFound = true;
//                break;
//            }
//        }
//        if (!bDevFound) {
//            LogMsgF("!! cyfx3 of key %llx not found", (unsigned long long)hKeyTarget);
//            return -1;
//        }
//    }
//
//    LogMsgF("==== load firmware to device with key %lx", (unsigned long)hKeyTarget);
//    ar = AT3DRV_LDR_LoadFirmware(hKeyTarget);
//    CHK_AR(ar, "load");
//
//#if 1
//    // do not wait here in UI thread.
//    // usb device detach & re-attach event will come from android system.
//#else
//    LogMsgF("==== wait until fx reboot..");
//    E_AT3_DEV_TYPE dt;
//    for (i=0; i<50; i++) {
//        // ar = AT3DRV_LDR_FindDeviceByType(eDT_AtlasFx3, hKey)
//        ar = AT3DRV_LDR_CheckDeviceExist(hKey, &dt);
//        if (AT3OK(ar) && dt == eDT_AtlasFx3) {
//            printf("ok, atfx3 detected\n");
//            mKey = hKey;
//            return 0;
//        }
//        printf("."); fflush(stdout);
//        AT3_DelayMs(100);
//    }
//    LogMsgF("dev not detected!");
//#endif
//    return 0;
//}

//jjustman-2020-08-24 - end copying from baseline impl



int LowaSISPHYAndroid::run()
{
    return 0;
}

bool LowaSISPHYAndroid::is_running() {

    return processThreadIsRunning && captureThreadIsRunning && statusThreadIsRunning;
}
/*
 * stop()
 *
 * unpin and tear down our threads in the following order:
 *
 *      captureThreadShouldRun
 *      statusThreadShouldRun
 *      processThreadShouldRun
 *
 *      and make sure to .join() to prevent std::terminate being called in the ~thread()
 */

#define __LOWASIS_STOP_USLEEP_DURATION_MS 1000000
int LowaSISPHYAndroid::stop()
{
    AT3RESULT ar;
    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::stop: enter with this: %p, instance_is_preboot_device: %d, init_completed: %d, mhDevice: %p, captureThreadIsRunning: %d, statusThreadIsRunning: %d, processThreadIsRunning: %d, sleeping for %d ms",
            this, this->instance_is_preboot_device, init_completed, mhDevice,
              this->captureThreadIsRunning,
              this->statusThreadIsRunning,
              this->processThreadIsRunning,
              __LOWASIS_STOP_USLEEP_DURATION_MS);

    //jjustman-2020-09-30 - immediately set our semaphores to wind down worker threads, in case we miss the callback in AT3DRV_WaitRxData/AT3DRVHandleRxData

    this->captureThreadShouldRun = false;
    this->statusThreadShouldRun = false;
    this->processThreadShouldRun = false;

    if(!this->instance_is_preboot_device) {
        //jjustman-2020-10-06 - give us 1s to allow callbacks to wind down
        usleep(__LOWASIS_STOP_USLEEP_DURATION_MS);
    }

    //tear down captureThread first, this will prevent any blocking calls to AT3DRV_WaitRxData or callback invocations into (volatile) this w/ AT3DRV_HandleRxData
    //make sure we unwind our captureThread and have exited AT3DRV_WaitRxData as per at3drv_api.h

    while(this->captureThreadIsRunning) {
        usleep(100000);
        _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: this->captureThreadIsRunning: %d", this->captureThreadIsRunning);
    }

    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: before join for captureThreadHandle");
    if(captureThreadHandle.joinable()) {
        captureThreadHandle.join();
    }
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: after join for captureThreadHandle");


    while(this->statusThreadIsRunning) {
        usleep(100000);
        _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: this->statusThreadIsRunning: %d", this->statusThreadIsRunning);
    }

    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: before join for statusThreadHandle");
    if(statusThreadHandle.joinable()) {
        statusThreadHandle.join();
    }
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: after join for statusThreadHandle");

    if(processThreadIsRunning) {
        //failsafe if our procesThread hasn't unwound yet...
        //unlock our producer thread, RAII scoped block to notify
        {
            lock_guard<mutex> lowasis_phy_rx_data_buffer_queue_guard(lowasis_phy_rx_data_buffer_queue_mutex);
            lowasis_phy_rx_data_buffer_condition.notify_one();
        }

        while(this->processThreadIsRunning) {
            usleep(100000);
            _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: this->processThreadIsRunning: %d", this->processThreadIsRunning);
        }
    }

    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: before join for processThreadHandle");
    if(processThreadHandle.joinable()) {
        processThreadHandle.join();
    }
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::stop: after join for processThreadHandle");

    if(mhDevice) {
        _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::stop: with this: %p, before AT3DRV_CancelWait with mhDevice: %p",
                                  this,
                                  mhDevice);

        ar = AT3DRV_CancelWait(mhDevice);
        if (ar) {
            _LOWASIS_PHY_ANDROID_WARN("AT3DRV_CancelWait:: with mhDevice: %p returned ar: %d", mhDevice, ar);
        }
        _LOWASIS_PHY_ANDROID_DEBUG("AT3DRV_CancelWait:: cancelled");

        ar = AT3DRV_FE_Stop(mhDevice);
        if (ar) {
            _LOWASIS_PHY_ANDROID_WARN("AT3DRV_FE_Stop:: with mhDevice: %p returned ar: %d", mhDevice, ar);
        }

        ar = AT3DRV_CloseDevice(mhDevice);
        if (ar) {
            _LOWASIS_PHY_ANDROID_WARN("AT3DRV_CloseDevice:: with mhDevice: %p returned ar: %d", mhDevice, ar);
        }
    }

    if(mAt3Opt) {
        ar = AT3DRV_Option_Release(mAt3Opt);
        if(ar) {
            _LOWASIS_PHY_ANDROID_WARN("AT3DRV_Option_Release:: with mAt3Opt: %p returned ar: %d", mAt3Opt, ar);
        }
    }

    if(init_completed) {
        AT3DRV_Uninit();
    }
    init_completed = false;

    mAt3Opt = nullptr;
    mhDevice = 0;
    this->instance_is_preboot_device = false;

    // clear ip/port statistics
    resetStatstics();

    if(atsc3_ndk_application_bridge_get_instance()) {
        atsc3_ndk_application_bridge_get_instance()->atsc3_phy_notify_plp_selection_change_clear_callback();
    }

    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::stop: return with this: %p", this);
    return ar;
}

void LowaSISPHYAndroid::resetStatstics() {
    s_ulLastTickPrint = 0;
    s_ullTotalBytes = s_ullTotalPkts = 0;
    s_uTotalLmts = 0;
    s_mapIpPort.clear();
    s_nPrevLmtVer = -1;
    s_ulL1SecBase = 0;
}

/*
 * jjustman-2020-08-23: NOTE - do NOT call delete lowaSISPHYAndroid* from anywhere else,
 *      only call deinit() otherwise you will get fortify crashes, ala:
 *  08-24 08:29:32.717 18991 18991 F libc    : FORTIFY: pthread_mutex_destroy called on a destroyed mutex (0x783b5c87b8)
 */

int LowaSISPHYAndroid::deinit()
{
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::deinit: enter with this: %p", this);
    this->stop();
    delete this;
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::deinit: return after delete this, with this: %p", this);

    return 0;
}

int LowaSISPHYAndroid::open(int fd, int device_type, string device_path)
{
    AT3RESULT ar;
    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::open, this: %p,  with fd: %d, device_path: %s", this, fd, device_path.c_str());

    ASSERT(init_completed, "not init");
    ASSERT(!mhDevice, "already open");

    //jjustman-2020-08-24 - hack?
    int nDevAdded;
    S_ETA_DEVICE sEtaDevice;
    sEtaDevice.fd = fd;
    sEtaDevice.devfs = device_path.c_str();

    ar = AT3DRV_LDR_PolulateUsbDevices(&sEtaDevice, 1, &nDevAdded);
    AT3_DEVICE hDevice = 0;

    if (1) {
        AT3_DEV_KEY keys[32];
        int nkey;
        bool bFound = false;

        printf("first, list all at3devices..\n");
        ar = AT3DRV_LDR_SearchDevicesByType(eDFF_Any, keys, 32, &nkey);
        CHK_AR(ar, "find at3dev");

        _LOWASIS_PHY_ANDROID_INFO("==== all at3 devices (%d)", nkey);
        for (int i=0; i<nkey; i++) {
            _LOWASIS_PHY_ANDROID_INFO(" key[%d]: %lx", i, (unsigned long)keys[i]);
        }
    }

    ar = AT3DRV_Option_Create(&mAt3Opt);
    CHK_AR(ar, "Option Create");


    //jjustman-2020-12-24 - testing sea 533 for plp0 and plp1 lock...
    //AT3UTL_SetAllDbgLevel(999);
    //AT3DRV_Option_SetInt(mAt3Opt, "output-bbp", 1);
    //AT3DRV_Option_SetInt(mAt3Opt, "output-alp", 1);

    AT3DRV_Option_SetInt(mAt3Opt, "output-ip", 1);
    AT3DRV_Option_SetInt(mAt3Opt, "get-parsed-lmt", 1);
    // if this set to 1, S_AT3DRV_RXDINFO_LMT::lmt is returned with valid data.
    // note that it is not efficient because every received lmt is parsed
    // regardless of actual lmt change (no checking of version or CRC).
    // if you can manage raw lmt data, it is better to parse by yourself.
    //AT3DRV_Option_SetInt(mAt3Opt, "fx-ep2-watchdog", 20);

    //jjustman-2019-11-01 - force ALP standard to USA, this addresses issues with "auto" mode
    //and multi-PLP configuration and improper IP flow resolution
    AT3DRV_Option_SetString(mAt3Opt, "alp-standard", "usa");
    AT3_DEV_KEY hKeyTarget = 0; //hack

    ar = AT3DRV_OpenDevice(&hDevice, hKeyTarget, mAt3Opt);
    CHK_AR(ar, "OpenDevice");

    mhDevice = hDevice;

    //jjustman-2020-12-25 - keep track of our FE PHY Demod Vendor (e.g. LG, Sony, etc..)
    ar = AT3DRV_FE_GetStatus(mhDevice, eAT3_FESTAT_HW_INFO, (S_AT3_FE_INFO*)&this->phyFeVendorDemodInfo);
    if (!AT3OK(ar)) {
        _LOWASIS_PHY_ANDROID_WARN("LowaSISPHYAndroid::init - AT3DRV_FE_GetStatus w/ eAT3_FESTAT_VENDOR returned not ok! %d", ar);
    }

    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::init - PHY info: vendor: %s, hwrev: 0x%08x, fwrev: %s", getPhyFeVendorNameString(this->phyFeVendorDemodInfo.vendor), this->phyFeVendorDemodInfo.hwrev, this->phyFeVendorDemodInfo.fwrev);

    //check if we were re-initalized and might have an open RxThread or RxStatusThread

    if(captureThreadHandle.joinable()) {
        captureThreadShouldRun = false;
        _LOWASIS_PHY_ANDROID_INFO("::Open() - setting captureThreadShouldRun to false, Waiting for captureThreadHandle to join()");
        captureThreadHandle.join();
    }

    if(processThreadHandle.joinable()) {
        processThreadShouldRun = false;
        _LOWASIS_PHY_ANDROID_INFO("::Open() - setting processThreadShouldRun to false, Waiting for processThreadHandle to join()");
        processThreadHandle.join();
    }

    if(statusThreadHandle.joinable()) {
        statusThreadShouldRun = false;
        _LOWASIS_PHY_ANDROID_INFO("::Open() - setting statusThreadShouldRun to false, Waiting for statusThreadHandle to join()");
        statusThreadHandle.join();
    }

    captureThreadShouldRun = true;
    captureThreadHandle = std::thread([this](){
        this->captureThread();
    });

    processThreadShouldRun = true;
    processThreadHandle = std::thread([this](){
        this->processThread();
    });

    statusThreadShouldRun = true;
    statusThreadHandle = std::thread([this]() {
        this->statusThread();
    });

    _LOWASIS_PHY_ANDROID_DEBUG("::open success: %p, returning 0", this);
    return 0;

ERROR:
    _LOWASIS_PHY_ANDROID_ERROR("::open failed: %p, returning -1", this);
    return -1;
}


int LowaSISPHYAndroid::tune(int freqKHz, int plpid)
{
    int ret = 0;

    AT3RESULT ar;

    _LOWASIS_PHY_ANDROID_DEBUG("%s (%d KHz, plp %d)", __func__, freqKHz, plpid);
    ASSERT(init_completed, "not init");
    ASSERT(mhDevice, "not open");

    if (plpid < 0 || plpid >= 64) {
        _LOWASIS_PHY_ANDROID_ERROR("LowaSISPHYAndroid::tune - invalid plp! value: %d", plpid);
        return -1;
    }

    //clear out our atsc3_core_service_player_bridge context
    atsc3_core_service_application_bridge_reset_context();

    //reset our last cached reference for LMT
    s_nPrevLmtVer = -1;

    ar = AT3DRV_FE_Start(mhDevice, freqKHz, eAT3_DEMOD_ATSC30, plpid);
    CHK_AR(ar, "FE_Start");

    _LOWASIS_PHY_ANDROID_DEBUG("tuned to freq %d MHz, plp %d", freqKHz/1000, plpid);

    is_tuned = true;

    return 0;
}

/*
 * jjustman-2020-08-24 -  always listen to plp0, so listen_plps may be a maximum of 3 values (4 if 0 is included)
 */
int LowaSISPHYAndroid::listen_plps(vector<uint8_t> plps_original_list)
{
    int ret = 0;
    uint8_t u_plp_ids[4] = { 0x40, 0x40, 0x40, 0x40 };
    int plp_postion = 0;

    if(!plps_original_list.size()) {
        _LOWASIS_PHY_ANDROID_ERROR("LowaSISPHYAndroid::listen_plps - size is 0, bailing!");
        return -1;
    }

    //jjustman-2020-02-29 - hack - check if FE vendor is LG3307_R850, only listen to the FIRST PLP listed in the index
    if(this->phyFeVendorDemodInfo.vendor == eAT3_FEVENDOR_LGDT3307_R850) {
        u_plp_ids[0] = plps_original_list.at(0);

        AT3DRV_FE_SetPLP(mhDevice, u_plp_ids, 1);
        _LOWASIS_PHY_ANDROID_INFO("listen_plps: LG3307_R850: setting to SINGLE plp_id[0]: %d", u_plp_ids[0]);
    } else {

        for(int i=0; i < plps_original_list.size() && plp_postion < 3; i++) {
            u_plp_ids[plp_postion++] = plps_original_list.at(i);
        }

#ifdef __JJUSTMAN_2020_12_24_SEA_533_SINGLE_PLP_TUNE_ONLY__
        u_plp_ids[0] = u_plp_ids[plp_postion-1];

        _LOWASIS_PHY_ANDROID_INFO("listen_plps: forcing to SINGLE plp_id[0]: %d (#defined __JJUSTMAN_2020_12_24_SEA_533_SINGLE_PLP_TUNE_ONLY__)", u_plp_ids[0]);
        AT3DRV_FE_SetPLP(mhDevice, u_plp_ids, 1);
#else
        //jjustman-2021-02-03 - otherwise, listen to all PLPs provided (up to 4)
        AT3DRV_FE_SetPLP(mhDevice, u_plp_ids, plp_postion);

        _LOWASIS_PHY_ANDROID_INFO("listen_plps: MultiPLP count %d, plp_id[0]: %d, plp_id[1]: %d, plp_id[2]: %d, plp_id[3]: %d",
                                   plp_postion, u_plp_ids[0], u_plp_ids[1], u_plp_ids[2], u_plp_ids[3]);
#endif
    }


    return ret;
}

int LowaSISPHYAndroid::download_bootloader_firmware(int fd, int device_type, string device_path) {
    AT3RESULT ar;
    AT3_DEV_KEY toInitTarget;

    _LOWASIS_PHY_ANDROID_DEBUG("download_bootloader_firmware, this: %p, devicePath: %s, fd: %d", this, device_path.c_str(), fd);
    this->instance_is_preboot_device = true;

    //jjustman-2020-08-24 - hack?
    int nDevAdded;
    S_ETA_DEVICE sEtaDevice;
    sEtaDevice.fd = fd;
    sEtaDevice.devfs = device_path.c_str();

    ar = AT3DRV_LDR_PolulateUsbDevices(&sEtaDevice, 1, &nDevAdded);

    if (1) {
        int i, nkey = 0;
        AT3_DEV_KEY keys[32];
        bool bDevFound = false;

        ar = AT3DRV_LDR_SearchDevicesByType(eDFF_CypressFx3, keys, 32, &nkey);
        CHK_AR(ar, "find cyfx3");

        if (nkey <= 0) {
            _LOWASIS_PHY_ANDROID_DEBUG("download_bootloader_firmware:no cyfx3 detected");
            return 0;
        }
        _LOWASIS_PHY_ANDROID_DEBUG("download_bootloader_firmware: all cypress fx3 devices (%d)", nkey);
        for (i=0; i<nkey; i++) {
            _LOWASIS_PHY_ANDROID_DEBUG(" key[%d]: %lx", i, (unsigned long)keys[i]);
            if (true) {
                //just assume on android we have 1, so init this hKeyTarget == keys[i]) {
                toInitTarget = keys[i];
                bDevFound = true;
                break;
            }
        }
//        if (!bDevFound) {
//            _LOWASIS_PHY_ANDROID_DEBUG("download_bootloader_firmware: bDevFound is false for searching key %llx ", (unsigned long long)hKeyTarget);
//            return -1;
//        }
    }

    _LOWASIS_PHY_ANDROID_DEBUG("download_bootloader_firmware: this: %p, before calling AT3DRV_LDR_LoadFirmware %llx", this, (unsigned long)toInitTarget);
    ar = AT3DRV_LDR_LoadFirmware(toInitTarget);
    _LOWASIS_PHY_ANDROID_DEBUG("download_bootloader_firmware: this: %p, after calling AT3DRV_LDR_LoadFirmware %llx", this, (unsigned long)toInitTarget);

    CHK_AR(ar, "load");
    return ar == AT3RES_OK;
}

int LowaSISPHYAndroid::captureThread()
{
    AT3RESULT ar;
    _LOWASIS_PHY_ANDROID_DEBUG("%s, this: %p", __func__, this);
    pinProducerThreadAsNeeded();

    ASSERT(init_completed, "not init");
    ASSERT(mhDevice, "not open");

    this->captureThreadIsRunning = true;
    while (this->captureThreadShouldRun) {
        if (!is_tuned) {
            AT3_DelayMs(100);
            // user has better improve this using semaphore or event msg, instead of delay.
            continue;
        }

        ar = AT3DRV_WaitRxData(mhDevice, 500);
        //bail early
        if(!this->captureThreadShouldRun) {
            break;
        }

        if (ar == AT3RES_CANCEL) {
            _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::captureThread - wait cancelled");
            AT3_DelayMs(10);
            continue;
        }
        if (AT3ERR(ar)) {
            _LOWASIS_PHY_ANDROID_ERROR("LowaSISPHYAndroid::captureThread - wait rx data error %d (%s)", ar, AT3_ErrString(ar));
            if (ar != AT3RES_TIMEOUT)
                break;
        }
        ar = AT3DRV_HandleRxData(mhDevice, RxCallbackStatic, (uint64_t)this);
        if (AT3ERR(ar)) {
            _LOWASIS_PHY_ANDROID_ERROR("LowaSISPHYAndroid::captureThread - handle rx data error %d (%s)", ar, AT3_ErrString(ar));
        }
    }

    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::captureThread complete");

    this->releasePinnedProducerThreadAsNeeded();
    this->captureThreadIsRunning = false;

    return 0;
}

//re-scoping for AT3DRV_HandleRxData callback method
AT3RESULT LowaSISPHYAndroid::RxCallbackStatic(S_RX_DATA *pData, uint64_t ullUser)
{
    LowaSISPHYAndroid *me = (LowaSISPHYAndroid *)ullUser;
    if(me->captureThreadShouldRun) {
        return me->RxCallbackInstanceScoped(pData);
    } else {
        return AT3RES_CANCEL;
    }
}

//re-scoping for AT3DRV_RegisterL1DTimeInfoCallback and AT3DRV_L1D_TIME_INFO_CB fnCallback
AT3RESULT LowaSISPHYAndroid::L1DTimeInfoCallback(uint32_t sec, uint16_t msec, uint16_t usec, uint16_t nsec, uint64_t ullUser) {

    LowaSISPHYAndroid *me = (LowaSISPHYAndroid *)ullUser;
    if(me->statusThreadShouldRun) {
        //jjustman-2021-09-01 - we dont't have an accurate value for time_info_flag in this callback
        _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::L1DTimeInfoCallback: L1time: flag: %d, s: %d, ms: %d, us: %d, ns: %d", -1, sec, msec, usec, nsec);

        if(atsc3_ndk_phy_bridge_get_instance()) {
            atsc3_ndk_phy_bridge_get_instance()->atsc3_update_l1d_time_information(0x3, sec, msec, usec, nsec);
        }

        return AT3RES_OK;
    } else {
        return AT3RES_CANCEL;
    }
}

//jjustman-2020-08-24 - todo: only push this S_RX_DATA to our processing queue for consumerThread
AT3RESULT LowaSISPHYAndroid::RxCallbackInstanceScoped(S_RX_DATA *pData) {
    atsc3_lowasis_phy_android_rxdata_t* lowasis_phy_android_rxdata = atsc3_lowasis_phy_android_rxdata_duplicate_from_s_rx_data(pData);

    //jjustman-2020-09-30 - overflows inside the AT3DRV may cause us to fail to parse the ALP packet types, so check for null rxdata payload
    if(lowasis_phy_android_rxdata) {

        lock_guard<mutex> lowasis_phy_rx_data_buffer_queue_guard(lowasis_phy_rx_data_buffer_queue_mutex);
        lowasis_phy_rx_data_buffer_queue.push(lowasis_phy_android_rxdata);
        lowasis_phy_rx_data_buffer_condition.notify_one();
    }
    return AT3RES_OK;
}

int LowaSISPHYAndroid::processThread()
{
    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::ProcessThread: with this: %p", this);
    this->pinConsumerThreadAsNeeded();
    this->processThreadIsRunning = true;

    queue<atsc3_lowasis_phy_android_rxdata_t*> to_process_queue; //perform a shallow copy so we can exit critical section asap
    atsc3_lowasis_phy_android_rxdata_t* pData = nullptr;

    while (this->processThreadShouldRun) {
        //enter critical section with condition wait
        {
            unique_lock<mutex> condition_lock(lowasis_phy_rx_data_buffer_queue_mutex);
            lowasis_phy_rx_data_buffer_condition.wait(condition_lock);

            while (lowasis_phy_rx_data_buffer_queue.size()) {
                to_process_queue.push(lowasis_phy_rx_data_buffer_queue.front());
                lowasis_phy_rx_data_buffer_queue.pop();
            }
            condition_lock.unlock();
        }

        //bail early if we should be shut down
        if(!this->processThreadShouldRun) {
            break;
        }

        //exit critical section, now we can process our to_process_queue
        while (to_process_queue.size()) {
            pData = to_process_queue.front();
            to_process_queue.pop();

            int nSizeMB;
            bool bShowStat = false;
            char buf[1024] = {0,};
            char tmp64[64], tmp128[128];

            if (pData->eType == eAT3_RXDTYPE_IP) {

#ifdef __ATSC3_LOWASIS_PENDANTIC__
                _LOWASIS_PHY_ANDROID_TRACE("::processThread() - packetLen: %d", pData->payload->p_size);
#endif
                S_AT3DRV_RXDINFO_IP *info = (S_AT3DRV_RXDINFO_IP *) pData->pInfo;

#ifdef __LOWASIS_USE_PROCESS_THREAD_RXDATA_L1D_TIME_FOR_UPDATE
                if (info->l1time.flag) { // dump L1D time info.
                    _LOWASIS_PHY_ANDROID_DEBUG("LowaSISPHYAndroid::ProcessThread: L1time: flag: %d, s: %d, ms: %d, us: %d, ns: %d",
                                               info->l1time.flag, info->l1time.sec, info->l1time.msec, info->l1time.usec, info->l1time.nsec);

                    if(atsc3_ndk_phy_bridge_get_instance()) {
                        atsc3_ndk_phy_bridge_get_instance()->atsc3_update_l1d_time_information(info->l1time.flag, info->l1time.sec, info->l1time.msec, info->l1time.usec, info->l1time.nsec);
                    }
                }
#endif
                if (atsc3_phy_rx_udp_packet_process_callback) {
                    atsc3_phy_rx_udp_packet_process_callback(info->plp_id, pData->payload);  //make sure to call atsc3_lowasis_phy_android_rxdata_free later
                }

                s_ullTotalPkts += 1;
                s_ullTotalBytes += pData->payload->p_size;

                //        if (info->b_discon) bShowStat = true;
                //        if (info->l1time.flag) bShowStat = true;

                if ((int32_t) (pData->ulTick - s_ulLastTickPrint) >= 1200) bShowStat = true;

                if (pData->payload->p_size > 22) {
                    uint8_t *p = pData->payload->p_buffer;
                    sprintf(tmp64, "%d.%d.%d.%d:%d", p[16], p[17], p[18], p[19],
                            ((uint16_t) p[22] << 8) | p[23]);
                    std::string ipport = tmp64;

                    auto it = s_mapIpPort.find(ipport);
                    if (it != s_mapIpPort.end())
                        it->second += 1;
                    else
                        s_mapIpPort[ipport] = 1; // add new ip/port into map
                }

#ifdef __LOTS_OF_PHY_DEBUGGING__
                if (bShowStat) {
                    sprintf(buf, "==== IP: ");
                    nSizeMB = (int)(s_ullTotalBytes/1024/1024);
                    sprintf(buf+strlen(buf), "total %u pkts, %u MB received", (int)s_ullTotalPkts, nSizeMB);
                    if (info->b_discon)
                        sprintf(buf+strlen(buf), " (pkt dropped!)");
                    sprintf(buf+strlen(buf), ", %u lmt", (unsigned)s_uTotalLmts);
                    if (info->l1time.flag) { // print L1D time info.
                        if (!s_ulL1SecBase) s_ulL1SecBase = info->l1time.sec;
                        sprintf(buf+strlen(buf), ", %s", AT3_L1TimeString(&(info->l1time), tmp128, s_ulL1SecBase));
                    }
        //            printf("%s\n", buf);
                    strcat(buf, "\n");
                    int n = 0;
                    for(auto& x : s_mapIpPort) { // print ip/port pair info
        //                printf("     %s, %d\n", x.first.c_str(), x.second);
                        sprintf(buf+strlen(buf), "     %s, %d\n", x.first.c_str(), x.second);
                        // print max 10 ip/port pairs because it is fixed-sized buffer.
                        if (++n >= 10) {
        //                    printf(" ..truncated\n");
                            strcat(buf, " ..truncated\n");
                            break;
                        }
                    }
        //            printf("%s", buf);
                    api.LogMsg(buf);
                    s_ulLastTickPrint = ulTick;
                }
#endif

            } else if (pData->eType == eAT3_RXDTYPE_IP_LMT) {
                S_AT3DRV_RXDINFO_LMT *info = (S_AT3DRV_RXDINFO_LMT *) pData->pInfo;
                uint8_t nLmtVer;

                s_uTotalLmts++;
                nLmtVer = info->lmt_ver;
                if (nLmtVer != s_nPrevLmtVer) {
                    _LOWASIS_PHY_ANDROID_INFO("LMT changed, size: %d, version: %d, num_multicasts: %d", pData->payload->p_size, nLmtVer, info->lmt->num_mc);

                    atsc3_link_mapping_table_t *atsc3_link_mapping_table = atsc3_link_mapping_table_new();
                    atsc3_link_mapping_table->alp_additional_header_for_signaling_information_signaling_version = info->lmt_ver;

                    for (int i = 0; i < info->lmt->num_mc; i++) {

                        atsc3_link_mapping_table_plp_t* atsc3_link_mapping_table_plp = atsc3_link_mapping_table_plp_new();
                        atsc3_link_mapping_table_plp->PLP_ID = info->lmt->mc[i].plp_id;
                        atsc3_link_mapping_table_add_atsc3_link_mapping_table_plp(atsc3_link_mapping_table, atsc3_link_mapping_table_plp);

                        atsc3_link_mapping_table_multicast_t *atsc3_link_mapping_table_multicast = atsc3_link_mapping_table_multicast_new();

                        atsc3_link_mapping_table_multicast->src_ip_add = info->lmt->mc[i].src_ip_add;
                        atsc3_link_mapping_table_multicast->dst_ip_add = info->lmt->mc[i].dst_ip_add;

                        atsc3_link_mapping_table_multicast->src_udp_port = info->lmt->mc[i].src_udp_port;
                        atsc3_link_mapping_table_multicast->dst_udp_port = info->lmt->mc[i].dst_udp_port;

                        atsc3_link_mapping_table_multicast->sid_flag = info->lmt->mc[i].sid_flag;
                        atsc3_link_mapping_table_multicast->compressed_flag = info->lmt->mc[i].compressed_flag;

                        if (atsc3_link_mapping_table_multicast->sid_flag) {
                            atsc3_link_mapping_table_multicast->sid_flag = info->lmt->mc[i].sid;
                        }

                        if (atsc3_link_mapping_table_multicast->compressed_flag) {
                            atsc3_link_mapping_table_multicast->compressed_flag = info->lmt->mc[i].context_id;
                        }

                        _LOWASIS_PHY_ANDROID_INFO("LMT: update - adding multicast: dest: %u.%u.%u.%u:%u, PLP: %d", __toipandportnonstruct(atsc3_link_mapping_table_multicast->dst_ip_add, atsc3_link_mapping_table_multicast->dst_udp_port), atsc3_link_mapping_table_plp->PLP_ID);
                        atsc3_link_mapping_table_plp_add_atsc3_link_mapping_table_multicast(atsc3_link_mapping_table_plp, atsc3_link_mapping_table_multicast);
                    }

                    if (atsc3_phy_rx_link_mapping_table_process_callback) {
                        _LOWASIS_PHY_ANDROID_INFO("LMT: update - invoking atsc3_phy_rx_link_mapping_table_process_callback with atsc3_link_mapping_table: %p", atsc3_link_mapping_table);

                        atsc3_link_mapping_table_t *atsc3_link_mapping_table_to_free = atsc3_phy_rx_link_mapping_table_process_callback(atsc3_link_mapping_table);

                        if (atsc3_link_mapping_table_to_free) {
                            atsc3_link_mapping_table_free(&atsc3_link_mapping_table_to_free);
                        }
                    } else {
                        _LOWASIS_PHY_ANDROID_WARN("LMT: update - atsc3_phy_rx_link_mapping_table_process_callback is NULL!");
                    }

                    //AT3_HexDump(pData->ptr, pData->nLength);
                    // it dump to stderr which is not valid in android
                    if (info->lmt) {
                        AT3_ATSC_PrintLmt(info->lmt, 2);
                    }
                    s_nPrevLmtVer = nLmtVer;
                }
            } else if (pData->eType == eAT3_RXDTYPE_ALP) {
                S_AT3DRV_RXDINFO_ALP *info = (S_AT3DRV_RXDINFO_ALP *) pData->pInfo;

                _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::ProcessThread: loop: eAT3_RXDTYPE_ALP: plp_id: %d, packet_type: %d, header_len: %d, b_discon: %d, pData is: %p, pData->payload->p_size: %d\n",
                                          info->plp_id, info->packet_type, info->header_len, info->b_discon, pData, pData->payload->p_size);

                //output delay for logging
                if ((int32_t) (pData->ulTick - s_ulLastTickPrint) >= 2000) {
                    bShowStat = true;
                    s_ulLastTickPrint = pData->ulTick;
                }

                // update statistics
                s_ullTotalPkts += 1;
                s_ullTotalBytes += pData->payload->p_size;

                if (bShowStat) {
                    nSizeMB = (int) (s_ullTotalBytes / 1024 / 1024);
                    printf("ALP received total %u pkts, %u MB received %s\n",
                           (int) s_ullTotalPkts, nSizeMB,
                           info->b_discon ? "(pkt dropped)" : "");
                    s_ulLastTickPrint = pData->ulTick;
                }
            } else if (pData->eType == eAT3_RXDTYPE_BBPCTR) {
                S_AT3DRV_RXDINFO_BBPCTR *info = (S_AT3DRV_RXDINFO_BBPCTR *) pData->pInfo;

                _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::ProcessThread: loop: eAT3_RXDTYPE_BBPCTR: b_discon: %d, pData is: %p, pData->payload->p_size: %d\n",
                                           info->bDiscontinuity, pData, pData->payload->p_size);

                if ((int32_t) (pData->ulTick - s_ulLastTickPrint) >= 2000) {
                    bShowStat = true;
                }

                // update statistics
                s_ullTotalPkts += 1;
                s_ullTotalBytes += pData->payload->p_size;

                if (info->bDiscontinuity)
                    bShowStat = true;

                if (bShowStat) {
                    nSizeMB = (int) (s_ullTotalBytes / 1024 / 1024);
                    printf("BBP received total %u pkts, %u MB received %s\n", (int) s_ullTotalPkts, nSizeMB,
                           info->bDiscontinuity ? "(pkt dropped)" : "");
                    s_ulLastTickPrint = pData->ulTick;
                }
            } else {
                printf("!! unknown type %d\n", pData->eType);
            }

            atsc3_lowasis_phy_android_rxdata_free(&pData);
        }
    }

    if(!this->processThreadShouldRun) {
        //clear out any pending/discards
        lock_guard<mutex> lowasis_phy_rx_data_buffer_queue_guard(lowasis_phy_rx_data_buffer_queue_mutex);

        while (lowasis_phy_rx_data_buffer_queue.size()) {
            pData = lowasis_phy_rx_data_buffer_queue.front();
            atsc3_lowasis_phy_android_rxdata_free(&pData);
            lowasis_phy_rx_data_buffer_queue.pop();
        }
    }

    this->releasePinnedConsumerThreadAsNeeded();
    this->processThreadIsRunning = false;

    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::processThread complete");
    return 0;
}

/*
 * jjustman-2020-12-24 - todo - add in other phy metrics, e.g.
 *
 * S_AT3_PHY_L1D_COMMON
 *
 * eAT3_FESTAT_L1BASIC
 *
 * eAT3_FESTAT_L1PREAMBLE
 *
 * eAT3_FESTAT_BOOTSTRAP_PARAM
 *
 *
 * dead code?
 *

//            uint8_t modcod_valid = s_fe_detail.aFecModCod[0].valid;
//            uint8_t E_L1d_PlpFecType = s_fe_detail.aFecModCod[0].fecType;
//            uint8_t E_L1d_PlpMod = s_fe_detail.aFecModCod[0].mod;
//            uint8_t E_L1d_PlpCod = s_fe_detail.aFecModCod[0].cod;
//
//            if(!modcod_valid) {
//                //try fallback:
//
//                //eAT3_FESTAT_LGD_PLP_V1
//                S_LGD_L2_PLPINFO* l2plpInfo = (S_LGD_L2_PLPINFO*)calloc(1, sizeof(S_LGD_L2_PLPINFO));
//                l2plpInfo->index = 0;
//                AT3DRV_FE_GetStatus(mhDevice, eAT3_FESTAT_LGD_PLP_V1, l2plpInfo);
//                modcod_valid = 1;
//                E_L1d_PlpFecType = l2plpInfo->plp_fec_type;
//                E_L1d_PlpMod = l2plpInfo->plp_mod;
//                E_L1d_PlpCod = l2plpInfo->plp_cr;
//                free(l2plpInfo);
//            }
 */

int LowaSISPHYAndroid::statusThread()
{
    AT3RESULT ar;

    S_AT3_PHY_BSP            s_fe_bootstrap = { '0' };
    S_FE_DETAIL              s_fe_detail    = { '0' };
    S_LGDEMOD_L2_SIG_STATUS  s_fe_detail_lg = { '0' };

    atsc3_ndk_phy_client_rf_metrics_t atsc3_ndk_phy_client_rf_metrics = { '0' };

    _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::statusThread started, this: %p", this);
    this->pinStatusThreadAsNeeded();
    this->statusThreadIsRunning = true;

    AT3DRV_RegisterL1DTimeInfoCallback(mhDevice, L1DTimeInfoCallback, (uint64_t)this);

    while(this->statusThreadShouldRun) {
        usleep(500000);
        if(!this->statusThreadShouldRun) {
            break;
        }

        if(this->is_tuned) {
            int32_t lock = 1, rssi = -2000;

            memset(&s_fe_detail, 0, sizeof(s_fe_detail));
            memset(&s_fe_detail_lg, 0, sizeof(s_fe_detail_lg));

            //jjustman-2020-12-25 - lg 3307 has different AT3DRV_FE_GetStatus type/struct for RF _and_ PLP statistics
            s_fe_detail.flagRequest = 0xffffffff; // all info. too many?
            //s_fe_detail.flagRequest = FE_SIG_MASK_Lock; // | FE_SIG_MASK_RfLevel | FE_SIG_MASK_CarrierOffset | FE_SIG_MASK_SNR | FE_SIG_MASK_BER | FE_SIG_MASK_FecModCod | FE_SIG_MASK_BbpErr;
            AT3DRV_FE_GetStatus(mhDevice, eAT3_FESTAT_RF_DETAIL, &s_fe_detail);
            _LOWASIS_PHY_ANDROID_INFO("LowaSISPHYAndroid::statusThread: s_fe_detail: 0x%08x, s_fe_detail.sBootstrap.eaWakeUp is: 0x%02x", s_fe_detail.flagReturned, s_fe_detail.sBootstrap.eaWakeUp);

            if(this->phyFeVendorDemodInfo.vendor == eAT3_FEVENDOR_LGDT3307_R850) {

                ar = AT3DRV_FE_GetStatus(mhDevice, eAT3_FESTAT_LGD_SIGSTAT_V1, &s_fe_detail_lg);

                //missing tuner_lock
                //missing rssi

                atsc3_ndk_phy_client_rf_metrics.demod_lock = s_fe_detail_lg.demodLock;

                /* missing? */
                atsc3_ndk_phy_client_rf_metrics.plp_lock_any = s_fe_detail.lock.bPlpLockAny;
                atsc3_ndk_phy_client_rf_metrics.plp_lock_all = s_fe_detail.lock.bPlpLockAll;
                atsc3_ndk_phy_client_rf_metrics.plp_lock_by_setplp_index = s_fe_detail.lock.bmPlpLock;

                atsc3_ndk_phy_client_rf_metrics.rfLevel1000 = s_fe_detail.nRfLevel1000;
                atsc3_ndk_phy_client_rf_metrics.snr1000_global = s_fe_detail_lg.snr * 1000;

                for(int i=0; i < 4; i++) {
                    S_LGD_L2_PLPINFO lgL2PlpInfo = { 0 };
                    lgL2PlpInfo.index = i;
                    AT3DRV_FE_GetStatus(mhDevice, eAT3_FESTAT_LGD_PLP_V1, &lgL2PlpInfo);

                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_id         = lgL2PlpInfo.l1d_plp_id;

                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].modcod_valid   = 1; //jjustman-2020-12-25 - not present in lgL2PlpInfo;
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_fec_type   = lgL2PlpInfo.plp_fec_type;
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_mod        = lgL2PlpInfo.plp_mod;
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_cod        = lgL2PlpInfo.plp_cr;

                    //jjustman-2020-12-25 - todo: investigate if this is returned for LG3307 in lowaSIS API
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].ber_pre_ldpc   = s_fe_detail_lg.ber; // pass in aggregate BER? s_fe_detail.aBerPreLdpcE7[i];
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].ber_pre_bch    = 0; //s_fe_detail.aBerPreBchE9[i];
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].fer_post_bch   = 0; //s_fe_detail.aFerPostBchE6[i];
                }

            } else {

                ar = AT3DRV_FE_GetStatus(mhDevice, eAT3_RFSTAT_LOCK, &lock);
                atsc3_ndk_phy_client_rf_metrics.tuner_lock = lock;

                ar = AT3DRV_FE_GetStatus(mhDevice, eAT3_RFSTAT_STRENGTH, &rssi);
                atsc3_ndk_phy_client_rf_metrics.rssi = rssi;

                //s_fe_detail.flagRequest = FE_SIG_MASK_Lock; // | FE_SIG_MASK_RfLevel | FE_SIG_MASK_CarrierOffset | FE_SIG_MASK_SNR | FE_SIG_MASK_BER | FE_SIG_MASK_FecModCod | FE_SIG_MASK_BbpErr;

                atsc3_ndk_phy_client_rf_metrics.demod_lock = s_fe_detail.lock.bDemodLock;

                atsc3_ndk_phy_client_rf_metrics.plp_lock_any = s_fe_detail.lock.bPlpLockAny;
                atsc3_ndk_phy_client_rf_metrics.plp_lock_all = s_fe_detail.lock.bPlpLockAll;
                atsc3_ndk_phy_client_rf_metrics.plp_lock_by_setplp_index = s_fe_detail.lock.bmPlpLock;

                atsc3_ndk_phy_client_rf_metrics.rfLevel1000 = s_fe_detail.nRfLevel1000;
                atsc3_ndk_phy_client_rf_metrics.snr1000_global = s_fe_detail.nSnr1000;

                for(int i=0; i < 4; i++) {
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_id         = s_fe_detail.idPlps[i];

                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].modcod_valid   = s_fe_detail.aFecModCod[i].valid;
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_fec_type   = s_fe_detail.aFecModCod[i].fecType;
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_mod        = s_fe_detail.aFecModCod[i].mod;
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].plp_cod        = s_fe_detail.aFecModCod[i].cod;

                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].ber_pre_ldpc   = s_fe_detail.aBerPreLdpcE7[i];
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].ber_pre_bch    = s_fe_detail.aBerPreBchE9[i];
                    atsc3_ndk_phy_client_rf_metrics.phy_client_rf_plp_metrics[i].fer_post_bch   = s_fe_detail.aFerPostBchE6[i];
                }
            }

            if(atsc3_ndk_phy_bridge_get_instance()) {
                atsc3_ndk_phy_bridge_get_instance()->atsc3_update_rf_stats_from_atsc3_ndk_phy_client_rf_metrics_t(&atsc3_ndk_phy_client_rf_metrics);
                atsc3_ndk_phy_bridge_get_instance()->atsc3_update_rf_bw_stats(s_ullTotalPkts, s_ullTotalBytes, s_uTotalLmts);
            }
        }
    }

    this->releasePinnedStatusThreadAsNeeded();
    this->statusThreadIsRunning = false;

    return 0;
}

void LowaSISPHYAndroid::NotifyPlpSelectionChangeCallback(vector<uint8_t> plps, void *context) {
    ((LowaSISPHYAndroid *) context)->listen_plps(plps);
}


extern "C"
JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_init(JNIEnv *env, jobject instance) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    _LOWASIS_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_init: start init, env: %p", env);
    if(lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_init: start init, LowaSISPHYAndroid is present: %p, calling deinit/delete", lowaSISPHYAndroid);
        lowaSISPHYAndroid->deinit();
        lowaSISPHYAndroid = nullptr;
    }

    lowaSISPHYAndroid = new LowaSISPHYAndroid(env, instance);
    lowaSISPHYAndroid->init();

    _LOWASIS_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_init: return, instance: %p", lowaSISPHYAndroid);
    return 0;
}


extern "C"
JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_run(JNIEnv *env, jobject thiz) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_run: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = -1;
    } else {
        res = lowaSISPHYAndroid->run();
        _LOWASIS_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_run: returning res: %d", res);
    }
    return res;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_is_1running(JNIEnv* env, jobject instance)
{
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);
    jboolean res = false;

    if(!lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_is_1running: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = false;
    } else {
        res = lowaSISPHYAndroid->is_running();

    }
    return res;
}


extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_stop(JNIEnv *env, jobject thiz) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_stop: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = -1;
    } else {
        _LOWASIS_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_stop: enter with lowaSISPHYAndroid: %p", lowaSISPHYAndroid);

        res = lowaSISPHYAndroid->stop();
        _LOWASIS_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_stop: returning res: %d", res);
    }

    return res;
}

/*
    jjustman-2020-10-07 - hack for dangling lowaSIS libusb

    frame #1: 0x0000007d656aebcc libc.so`abort + 344
    frame #2: 0x00000073af6b08ec libc++_shared.so`::abort_message(format=<unavailable>) at abort_message.cpp:76
    frame #3: 0x00000073af6b0a08 libc++_shared.so`demangling_terminate_handler() at cxa_default_handlers.cpp:77
    frame #4: 0x00000073af6c2534 libc++_shared.so`std::__terminate(func=<unavailable>)()) at cxa_handlers.cpp:59
    frame #5: 0x00000073af6c24dc libc++_shared.so`std::terminate() at cxa_handlers.cpp:92
    frame #6: 0x00000073af6afdc8 libc++_shared.so`std::__ndk1::thread::~thread(this=<unavailable>) at thread.cpp:47
    frame #7: 0x00000073afe91454 libatsc3_phy_lowasis.so`LowaSISPHYAndroid::~LowaSISPHYAndroid(this=0x780000468845c180) at LowaSISPHYAndroid.cpp:61
    frame #8: 0x00000073afe915a8 libatsc3_phy_lowasis.so`LowaSISPHYAndroid::~LowaSISPHYAndroid(this=0x780000468845c180) at LowaSISPHYAndroid.cpp:42
    frame #9: 0x00000073afe99440 libatsc3_phy_lowasis.so`LowaSISPHYAndroid::deinit(this=0x780000468845c180) at LowaSISPHYAndroid.cpp:432

    wire up a std::exception handler for the ~lowaSISPHYAndroid chained constructure,
    as the terminate handler for the thrown exception of un-jointed thread from long-polling libusb async fo libsub_handle_events (60s) which is causing above crash

    see: http://libusb.sourceforge.net/api-1.0/libusb_mtasync.html for more details

 */


extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_deinit(JNIEnv *env, jobject thiz) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_deinit: error, srtRxSTLTPVirtualPHYAndroid is NULL!");
        res = -1;
    } else {
        _LOWASIS_PHY_ANDROID_INFO("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_deinit: enter with lowaSISPHYAndroid: %p", lowaSISPHYAndroid);

        try {
            lowaSISPHYAndroid->deinit();
            lowaSISPHYAndroid = nullptr;
        } catch (std::exception& e) {
            _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_deinit() - Caught exception in lowaSISPHYAndroid->deinit, ex is: %s", e.what());
        }

        _LOWASIS_PHY_ANDROID_INFO("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_deinit: exit with lowaSISPHYAndroid: %p", lowaSISPHYAndroid);
    }

    return res;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_download_1bootloader_1firmware(JNIEnv *env, jobject thiz, jint fd, jint device_type, jstring device_path_jstring) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    _LOWASIS_PHY_ANDROID_INFO("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_download_1bootloader_1firmware: fd: %d", fd);
    int res = 0;

    if(!lowaSISPHYAndroid)  {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_download_1bootloader_1firmware: LowaSISPHYAndroid is NULL!");
        res = -1;
    } else {
        const char* device_path_weak = env->GetStringUTFChars(device_path_jstring, 0);
        string device_path(device_path_weak);
        res = lowaSISPHYAndroid->download_bootloader_firmware(fd, device_type, device_path); //calls pre_init
        env->ReleaseStringUTFChars( device_path_jstring, device_path_weak );

        //jjustman-2020-08-23 - hack, clear out our in-flight reference since we should re-enumerate
        delete lowaSISPHYAndroid;
        lowaSISPHYAndroid = nullptr;
    }

    return res;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_open(JNIEnv *env, jobject thiz, jint fd, jint device_type, jstring device_path_jstring) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    _LOWASIS_PHY_ANDROID_DEBUG("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_open: fd: %d", fd);

    int res = 0;
    if(!lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_open: LowaSISPHYAndroid is NULL!");
        res = -1;
    } else {
        const char* device_path_weak = env->GetStringUTFChars(device_path_jstring, 0);
        string device_path(device_path_weak);
        res = lowaSISPHYAndroid->open(fd, device_type, device_path);
        env->ReleaseStringUTFChars( device_path_jstring, device_path_weak );
    }
    return res;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_tune(JNIEnv *env, jobject thiz,
                                                                      jint freq_khz,
                                                                      jint single_plp) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_tune: LowaSISPHYAndroid is NULL!");
        res = -1;
    } else {
        res = lowaSISPHYAndroid->tune(freq_khz, single_plp);
    }

    return res;
}
extern "C" JNIEXPORT jint JNICALL
Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_listen_1plps(JNIEnv *env,
                                                                              jobject thiz,
                                                                              jobject plps) {
    lock_guard<mutex> lowasis_phy_android_cctor_mutex_local(LowaSISPHYAndroid::CS_global_mutex);

    int res = 0;
    if(!lowaSISPHYAndroid) {
        _LOWASIS_PHY_ANDROID_ERROR("Java_org_ngbp_libatsc3_middleware_android_phy_LowaSISPHYAndroid_listen_1plps: LowaSISPHYAndroid is NULL!");
        res = -1;
    } else {
        vector<uint8_t> listen_plps;

        jobject jIterator = env->CallObjectMethod(plps, env->GetMethodID(env->GetObjectClass(plps), "iterator", "()Ljava/util/Iterator;"));
        jmethodID nextMid = env->GetMethodID(env->GetObjectClass(jIterator), "next", "()Ljava/lang/Object;");
        jmethodID hasNextMid = env->GetMethodID(env->GetObjectClass(jIterator), "hasNext", "()Z");

        while (env->CallBooleanMethod(jIterator, hasNextMid)) {
            jobject jItem = env->CallObjectMethod(jIterator, nextMid);
            jbyte jByte = env->CallByteMethod(jItem, env->GetMethodID(env->GetObjectClass(jItem), "byteValue", "()B"));
            listen_plps.push_back(jByte);
        }

        res = lowaSISPHYAndroid->listen_plps(listen_plps);
    }

    return res;
}