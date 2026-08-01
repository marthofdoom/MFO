Scriptname MFO_Trade extends Quest
{#21 econ bridge (Fable's ECON_PAPYRUS_PLAN). The DLL owns the DECISION (vendor
 resolution, sellables, buy candidates, quality ranking) and dispatches
 RunTrade(token) at this quest. This script does what native cannot without a
 CTD -- the merchant-safe read (and, later, the transaction) -- pulling its order
 from MFO-registered natives keyed by token, then reporting the result back.

 Phase 1: READ-ONLY probe. Read the vendor's gold + per-candidate stock through
 the barter-safe Papyrus path and hand them back; native logs the WOULD SELL /
 WOULD BUY plan. Zero transactions. The sell/buy loops arrive in Phase 2/3.}

; ── MFO-registered natives (return SYNCHRONOUSLY to the calling Papyrus frame,
;    unlike the DLL's fire-and-forget dispatch that reached us). ──────────────
ObjectReference Function GetVendorChest(int token)   global native
Actor           Function GetVendorActor(int token)   global native
Form[]          Function GetBuyCandidates(int token) global native
Bool            Function GetProbeOnly(int token)      global native
Function ReportProbe(int token, int vendorGold, int[] stock) global native

Function RunTrade(int token)
    ObjectReference chest = GetVendorChest(token)
    Actor vendor = GetVendorActor(token)
    if chest == none
        Debug.Trace("[MFO_Trade] token=" + token + " -- no chest, aborting probe")
        int[] empty = new int[1]
        ReportProbe(token, -1, empty)   ; native logs the failure + frees the token
        return
    endif

    ; Vendor funds: the merchant chest's Gold001 (barter-safe GetItemCount, the
    ; path C.O.I.N. proves crash-free) plus the actor's pocket gold.
    Form gold001 = Game.GetFormFromFile(0x0000000F, "Skyrim.esm")
    int vendorGold = chest.GetItemCount(gold001)
    if vendor
        vendorGold += vendor.GetGoldAmount()
    endif

    ; Per-candidate stock: native ranked the buy candidates and knows their value;
    ; we only answer "does this vendor stock it, and how many?" -- again just
    ; GetItemCount, never an inventory enumeration.
    Form[] cands = GetBuyCandidates(token)
    int[] stock = new int[128]
    int n = cands.Length
    int i = 0
    while i < n && i < 128
        if cands[i]
            stock[i] = chest.GetItemCount(cands[i])
        endif
        i += 1
    endwhile

    Debug.Trace("[MFO_Trade] token=" + token + " vendorGold=" + vendorGold + " cands=" + n)
    ReportProbe(token, vendorGold, stock)
EndFunction
