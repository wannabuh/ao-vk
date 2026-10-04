// The replacements, installed once randy31_orig.dll is loaded.
#include "native/native.h"
#include "native/cat_anim.h"
#include "native/cat_skin.h"
#include "native/scene.h"

namespace rnative {

void Install(HMODULE orig)
{
    static bool done = false;
    if (done || !orig)
        return;
    done = true;
    skin::Install(orig);
    anim::Install(orig);
    scene::Install(orig);
}

}  // namespace rnative
