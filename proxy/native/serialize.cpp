// serialize.dll bindings (serialize.h).
#include "native/serialize.h"

#include <type_traits>

namespace rnative::serialize {

const Api& Get()
{
    static const Api api = [] {
        Api a;
        HMODULE m = GetModuleHandleA("serialize.dll");
        if (!m) return a;
        auto get = [m](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
            return fn != nullptr;
        };
        a.complete =
            get(a.construct, "??0Serializable_c@fun@@QAE@XZ") &&
            get(a.constructFrom, "??0Serializable_c@fun@@QAE@PAVObjectArchive_c@1@@Z") &&
            get(a.destroy, "??1Serializable_c@fun@@UAE@XZ") &&
            get(a.getStream, "?GetStream@ObjectArchive_c@fun@@QAEPAVArchiveStream_c@2@XZ") &&
            get(a.addInt32, "?AddInt32@Message_c@fun@@QAE?AW4MsgErr_e@12@PBDJ@Z") &&
            get(a.addString, "?AddString@Message_c@fun@@QAE?AW4MsgErr_e@12@PBDABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z") &&
            get(a.findString, "?FindString@Message_c@fun@@QBE?AW4MsgErr_e@12@PBDPAV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@H@Z") &&
            get(a.addObject, "?AddObject@ArchiveStream_c@fun@@QAE?AW4MsgErr_e@Message_c@2@PBDPBVSerializable_c@2@@Z") &&
            get(a.findInt32, "?FindInt32@Message_c@fun@@QBE?AW4MsgErr_e@12@PBDPAJH@Z") &&
            get(a.findFloat, "?FindFloat@Message_c@fun@@QBE?AW4MsgErr_e@12@PBDPAMH@Z") &&
            get(a.findRgb, "?FindRGB@Message_c@fun@@QBE?AW4MsgErr_e@12@PBDPAVRGB_t@@H@Z") &&
            get(a.addFloat, "?AddFloat@Message_c@fun@@QAE?AW4MsgErr_e@12@PBDM@Z") &&
            get(a.addRgb, "?AddRGB@Message_c@fun@@QAE?AW4MsgErr_e@12@PBDABVRGB_t@@@Z") &&
            get(a.addBool, "?AddBool@Message_c@fun@@QAE?AW4MsgErr_e@12@PBD_N@Z") &&
            get(a.addVector3, "?AddVector3@Message_c@fun@@QAE?AW4MsgErr_e@12@PBDABVVector3_t@@@Z") &&
            get(a.addQuat, "?AddQuat@Message_c@fun@@QAE?AW4MsgErr_e@12@PBDABVQuaternion_t@@@Z") &&
            get(a.addMatrix4, "?AddMatrix4@Message_c@fun@@QAE?AW4MsgErr_e@12@PBDABVTMatrix4_t@@@Z") &&
            get(a.findVector3, "?FindVector3@Message_c@fun@@QBE?AW4MsgErr_e@12@PBDPAVVector3_t@@H@Z") &&
            get(a.findQuat, "?FindQuat@Message_c@fun@@QBE?AW4MsgErr_e@12@PBDPAVQuaternion_t@@H@Z") &&
            get(a.findMatrix4, "?FindMatrix4@Message_c@fun@@QBE?AW4MsgErr_e@12@PBDPAVTMatrix4_t@@H@Z");
        return a;
    }();
    return api;
}

}  // namespace rnative::serialize
