// The replacements, installed once randy31_orig.dll is loaded.
#include "native/native.h"
#include "native/cat_anim.h"
#include "native/cat_mesh.h"
#include "native/cat_query.h"
#include "native/cat_render.h"
#include "native/cat_skin.h"
#include "native/device.h"
#include "native/orig_api.gen.h"
#include "native/scene.h"
#include "native/viewport.h"
#include "native/refframe.h"
#include "native/visual.h"
#include "native/camera.h"
#include "native/mesh.h"
#include "native/mesh_data.h"
#include "native/occluder.h"
#include "native/cat_life.h"
#include "native/cat_pick.h"
#include "native/cat_data.h"
#include "native/cat_anim_data.h"
#include "native/bvolume.h"
#include "native/gone_trap.h"
#include "native/keyframe.h"
#include "native/sprite.h"
#include "native/shadowlands.h"
#include "native/color.h"
#include "native/pixfmt.h"
#include "native/lbitmap.h"
#include "native/helpers.h"

namespace rnative {

void Install(HMODULE orig)
{
    static bool done = false;
    if (done || !orig)
        return;
    done = true;
    InstallCrashLog();
    if (!orig::Init(orig))
        Log("some of randy31_orig's exports are missing - unknown client build");
    skin::Install(orig);
    anim::Install(orig);
    scene::Install(orig);
    catrender::Install(orig);
    catmesh::Install(orig);
    catquery::Install(orig);
    device::Install(orig);
    viewport::Install(orig);
    refframe::Install(orig);
    visual::Install(orig);
    camera::Install(orig);
    mesh::Install(orig);
    meshdata::Install(orig);
    occluder::Install(orig);
    catlife::Install(orig);
    catpick::Install(orig);
    catdata::Install(orig);
    cat_anim_data::Install(orig);
    bvolume::Install(orig);
    keyframe::Install(orig);
    sprite::Install(orig);
    shadowlands::Install(orig);
    color::Install(orig);
    pixfmt::Install(orig);
    lbitmap::Install(orig);
    helpers::Install(orig);
    gonetrap::Install(orig);                        // last: over what the replacements left
}

}  // namespace rnative
