#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(r_disable_envmap_reflections, false, "Nuts&Bolts/GPU", "Null the shared EnvMap cubemap handle instead of resolving it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REX_EXTERN(__imp__sub_823A7020);
REX_HOOK_RAW(sub_823A7020) {
    // Let the game build the shared EnvMap cube texture and stash its
    // pointer in dword_82FAC688 as normal.
    __imp__sub_823A7020(ctx, base);

    if (REXCVAR_GET(r_disable_envmap_reflections)) {
        // dword_82FAC688 has exactly 2 xrefs total: this write, and the
        // read in sub_82308968 (0x82308a60) that copies it into any
        // material slot resolving to "EnvMapShared". Null it here and
        // every such slot gets a null handle instead - reflections off.
        // base + guest_addr = host pointer; be<uint32_t> handles the
        // big-endian store (a no-op for zero, but keeps it typed correctly).
        *reinterpret_cast<rex::be<uint32_t>*>(base + 0x82FAC688) = 0;
    }
}