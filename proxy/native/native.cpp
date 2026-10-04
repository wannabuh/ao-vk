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

namespace rnative {

void Install(HMODULE orig)
{
    static bool done = false;
    if (done || !orig)
        return;
    done = true;
    if (!orig::Init(orig))
        Log("some of randy31_orig's exports are missing - unknown client build");
    skin::Install(orig);
    anim::Install(orig);
    scene::Install(orig);
    catrender::Install(orig);
    catmesh::Install(orig);
    catquery::Install(orig);
    device::Install(orig);
}

}  // namespace rnative
