Scriptname MFO_Trade extends Quest
{#21 econ bridge (Fable's ECON_PAPYRUS_PLAN). The DLL owns the DECISION (catalog,
 gambit quotas, quality ranking) and dispatches RunTrade(token) at this quest.
 This script does what native cannot without a CTD -- the merchant-safe read and
 the buy/sell transaction -- pulling its order from MFO-registered natives keyed
 by token, then reports the result back.

 Phase 0: prove the whole round trip end to end (DLL -> RunTrade -> NativePing ->
 DLL log) before any merchant is touched. The real accessors + loops land in the
 later phases; NativePing is the placeholder pull.}

; ── MFO-registered natives (return SYNCHRONOUSLY to the calling Papyrus frame,
;    unlike the DLL's fire-and-forget dispatch that reached us). ──────────────
Function NativePing(int token) global native

Function RunTrade(int token)
    Debug.Trace("[MFO_Trade] RunTrade token=" + token)
    NativePing(token)
EndFunction
