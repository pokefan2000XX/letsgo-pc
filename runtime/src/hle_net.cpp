// Network and wireless services, offline. Bluetooth LE (Poke Ball Plus and
// Pokemon GO pairing) finds no devices; local wireless (ldn) opens, hosts and
// scans with nobody nearby; BCAT delivery storage is empty; play reports and
// play history are accepted and dropped; socket and SSL libraries initialize
// without a network (nifm already reports none).
#include <mutex>

#include "hle.h"

namespace rt {
namespace net {

// ---- bluetooth (BLE) ----------------------------------------------------------------------
static void ble_event(Ctx* c) { attach_new_system_event(c, c->x[0], 1); c->x[0] = 0; }
static void ble_count0(Ctx* c) { c->x[0] = 0; }                       // lists: no entries
// GetBleScanParameter(GattAttributeUuid*, int id): uuid bytes at +4
static void GetBleScanParameter(Ctx* c) { memset((void*)(uintptr_t)(c->x[0] + 4), 0, 16); }

// ---- ldn (local wireless) -----------------------------------------------------------------
enum { kNone, kInitialized, kAccessPointOpened, kAccessPointCreated, kStationOpened, kStationConnected };
constexpr uint32_t kResultConnectNotFound = (65 << 9) | 203;          // 2203-0065
static std::mutex g_ldn_lock;
static int g_ldn_state = kNone;
static uint8_t g_ldn_config[0x20];                                    // NetworkConfig of our network
static uint32_t g_ldn_event = 0;

static void ldn_set(int s) {
    std::lock_guard<std::mutex> lk(g_ldn_lock);
    if (g_ldn_state == s) return;
    g_ldn_state = s;
    if (g_ldn_event) kevent_signal(g_ldn_event);
}
static void ldn_Initialize(Ctx* c) { ldn_set(kInitialized); c->x[0] = 0; }
static void ldn_Finalize(Ctx* c) { ldn_set(kNone); c->x[0] = 0; }
static void ldn_GetState(Ctx* c) { std::lock_guard<std::mutex> lk(g_ldn_lock); c->x[0] = (uint64_t)g_ldn_state; }
static void ldn_AttachStateChangeEvent(Ctx* c) { g_ldn_event = attach_new_system_event(c, c->x[0], 1); }
static void ldn_OpenAccessPoint(Ctx* c) { ldn_set(kAccessPointOpened); c->x[0] = 0; }
static void ldn_CloseAccessPoint(Ctx* c) { ldn_set(kInitialized); c->x[0] = 0; }
static void ldn_CreateNetwork(Ctx* c) {                               // (NetworkConfig const&, ...)
    memcpy(g_ldn_config, (const void*)(uintptr_t)c->x[0], sizeof g_ldn_config);
    ldn_set(kAccessPointCreated);
    c->x[0] = 0;
}
static void ldn_DestroyNetwork(Ctx* c) { ldn_set(kAccessPointOpened); c->x[0] = 0; }
static void ldn_OpenStation(Ctx* c) { ldn_set(kStationOpened); c->x[0] = 0; }
static void ldn_CloseStation(Ctx* c) { ldn_set(kInitialized); c->x[0] = 0; }
static void ldn_Disconnect(Ctx* c) { ldn_set(kStationOpened); c->x[0] = 0; }
static void ldn_Connect(Ctx* c) { c->x[0] = kResultConnectNotFound; }
// Scan(NetworkInfo*, int* count, int max, ScanFilter const&, int channel)
static void ldn_Scan(Ctx* c) { *(int32_t*)(uintptr_t)c->x[1] = 0; c->x[0] = 0; }
static void ldn_GetNetworkInfo(Ctx* c) { memset((void*)(uintptr_t)c->x[0], 0, 0x480); c->x[0] = 0; }
static void ldn_GetNetworkConfig(Ctx* c) { memcpy((void*)(uintptr_t)c->x[0], g_ldn_config, sizeof g_ldn_config); c->x[0] = 0; }
static void ldn_GetSecurityParameter(Ctx* c) { memset((void*)(uintptr_t)c->x[0], 0, 0x20); c->x[0] = 0; }
static void ldn_GetIpv4Address(Ctx* c) {
    *(uint32_t*)(uintptr_t)c->x[0] = 0xA9FE0101;                      // 169.254.1.1
    *(uint32_t*)(uintptr_t)c->x[1] = 0xFFFFFF00;
    c->x[0] = 0;
}

// ---- bcat, friends, prepo, ngc ------------------------------------------------------------
// EnumerateDeliveryCacheDirectory(int* count, DirectoryName*, int max)
static void bcat_Enumerate(Ctx* c) { *(int32_t*)(uintptr_t)c->x[0] = 0; c->x[0] = 0; }
// GetPlayHistoryRegistrationKey(PlayHistoryRegistrationKey*, Uid const&, bool)
static void friends_GetKey(Ctx* c) { memset((void*)(uintptr_t)c->x[0], 0, 0x40); c->x[0] = 0; }
}  // namespace net

void register_net_overrides(void (*reg)(const char*, rc_fn)) {
    using namespace net;
    reg("_ZN2nn9bluetooth13InitializeBleEv", hle_void);
    for (const char* s : {"_ZN2nn9bluetooth19AcquireBleScanEventEPNS_2os15SystemEventTypeE",
                          "_ZN2nn9bluetooth24AcquireBleMtuConfigEventEPNS_2os15SystemEventTypeE",
                          "_ZN2nn9bluetooth28AcquireBleGattOperationEventEPNS_2os15SystemEventTypeE",
                          "_ZN2nn9bluetooth31AcquireBleServiceDiscoveryEventEPNS_2os15SystemEventTypeE",
                          "_ZN2nn9bluetooth37AcquireBleConnectionStateChangedEventEPNS_2os15SystemEventTypeE"})
        reg(s, ble_event);
    for (const char* s : {"_ZN2nn9bluetooth16GetBleScanResultEPNS0_13BleScanResultEi",
                          "_ZN2nn9bluetooth24GetBleConnectionInfoListEPNS0_17BleConnectionInfoEi",
                          "_ZN2nn9bluetooth15GetGattServicesEPNS0_11GattServiceEij"})
        reg(s, ble_count0);
    reg("_ZN2nn9bluetooth19GetBleScanParameterEPNS0_17GattAttributeUuidEi", GetBleScanParameter);
    for (const char* s : {"_ZN2nn9bluetooth23StartBleScanSmartDeviceERKNS0_17GattAttributeUuidE",
                          "_ZN2nn9bluetooth22StopBleScanSmartDeviceEv",
                          "_ZN2nn9bluetooth19ConnectToGattServerERKNS0_7AddressE",
                          "_ZN2nn9bluetooth24DisconnectFromGattServerEj", "_ZN2nn9bluetooth15ConfigureBleMtuEjt",
                          "_ZN2nn9bluetooth22ReadGattCharacteristicERKNS0_18GattCharacteristicE",
                          "_ZN2nn9bluetooth23WriteGattCharacteristicERKNS0_18GattCharacteristicE",
                          "_ZN2nn9bluetooth19WriteGattDescriptorERKNS0_14GattDescriptorE",
                          "_ZN2nn9bluetooth33RegisterGattOperationNotificationERKNS0_17GattAttributeUuidE",
                          "_ZN2nn9bluetooth35UnregisterGattOperationNotificationERKNS0_17GattAttributeUuidE",
                          "_ZN2nn9bluetooth36EnableGattCharacteristicNotificationERKNS0_18GattCharacteristicEb"})
        reg(s, hle_ok);

    reg("_ZN2nn3ldn10InitializeEv", ldn_Initialize);
    reg("_ZN2nn3ldn8FinalizeEv", ldn_Finalize);
    reg("_ZN2nn3ldn8GetStateEv", ldn_GetState);
    reg("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_AttachStateChangeEvent);
    reg("_ZN2nn3ldn15OpenAccessPointEv", ldn_OpenAccessPoint);
    reg("_ZN2nn3ldn16CloseAccessPointEv", ldn_CloseAccessPoint);
    reg("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_CreateNetwork);
    reg("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
        ldn_CreateNetwork);
    reg("_ZN2nn3ldn14DestroyNetworkEv", ldn_DestroyNetwork);
    reg("_ZN2nn3ldn11OpenStationEv", ldn_OpenStation);
    reg("_ZN2nn3ldn12CloseStationEv", ldn_CloseStation);
    reg("_ZN2nn3ldn10DisconnectEv", ldn_Disconnect);
    reg("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_Connect);
    reg("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
        ldn_Connect);
    reg("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_Scan);
    reg("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_Scan);
    reg("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_GetNetworkInfo);
    reg("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", ldn_GetNetworkConfig);
    reg("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_GetSecurityParameter);
    reg("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_GetIpv4Address);
    reg("_ZN2nn3ldn19GetDisconnectReasonEv", hle_ok);                   // DisconnectReason_None
    reg("_ZN2nn3ldn16SetAdvertiseDataEPKvm", hle_ok);
    reg("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", hle_ok);
    reg("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", hle_ok);

    reg("_ZN2nn4bcat10InitializeEv", hle_void);
    reg("_ZN2nn4bcat25MountDeliveryCacheStorageEv", hle_ok);
    reg("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", hle_void);
    reg("_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", bcat_Enumerate);

    reg("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_GetKey);
    reg("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", hle_ok);

    reg("_ZN2nn5prepo10PlayReport4SaveEv", hle_ok);
    reg("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", hle_ok);
    reg("_ZN2nn5prepo28RequestImmediateTransmissionEv", hle_ok);

    reg("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", hle_ok);
    reg("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", hle_ok);
    reg("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", hle_ok);
    // no NG word list on the PC: every word is clean (the name check calls these)
    reg("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", [](Ctx* c) { c->x[0] = 1; });
    reg("_ZN2nn3ngc15ProfanityFilter15SkipAtSignCheckENS1_8SkipModeE", hle_void);
    reg("_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", hle_void);
    auto check = [](Ctx* c) {                     // CheckProfanityWords(u32* results, u32 patterns, words, count)
        uint32_t* out = (uint32_t*)(uintptr_t)c->x[1];
        for (uint64_t i = 0; out && i < c->x[4]; i++) out[i] = 0;
        c->x[0] = 0;
    };
    reg("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", check);
    reg("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", check);
    auto mask = [](Ctx* c) {                      // MaskProfanityWordsInText(int* count, text, flags)
        if (c->x[1]) *(int32_t*)(uintptr_t)c->x[1] = 0;
        c->x[0] = 0;
    };
    reg("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", mask);
    reg("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", mask);

    reg("_ZN2nn6socket10InitializeEPvmmi", hle_ok);
    reg("_ZN2nn6socket10InitializeERKNS0_6ConfigE", hle_ok);
    reg("_ZN2nn6socket8FinalizeEv", hle_ok);
    reg("nnsslInitialize", hle_ok);
    reg("nnsslFinalize", hle_ok);
}

}  // namespace rt
