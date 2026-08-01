Scriptname MFO_Trade extends Quest
{#21 econ bridge (Fable's ECON_PAPYRUS_PLAN). The DLL owns the DECISION (vendor
 resolution, sellables, catalog) and dispatches RunTrade(token) at this quest.
 This script does what native cannot without a CTD -- the merchant-safe read and
 the transaction -- pulling its order from MFO-registered natives keyed by token,
 then reporting the result back.

 Phase 2: SELL. Read the chest's barter gold, then (when bEconomy is on) sell the
 follower's junk to the vendor, highest-value first, capped at the chest's gold.
 bEconomy off = a dry run that computes what it WOULD sell without mutating. BUY
 arrives in Phase 3 (vendor-stock enumeration).}

; ── MFO-registered natives (return SYNCHRONOUSLY to the calling Papyrus frame,
;    unlike the DLL's fire-and-forget dispatch that reached us). ──────────────
Actor           Function GetTradeFollower(int token) global native
ObjectReference Function GetVendorChest(int token)   global native
Actor           Function GetVendorActor(int token)   global native
Bool            Function GetProbeOnly(int token)      global native
Form[]          Function GetSellForms(int token)      global native
Int[]           Function GetSellCounts(int token)     global native
Int[]           Function GetSellValues(int token)     global native
Function ReportTrade(int token, int soldValue, int soldCount, int vendorGold) global native

Function RunTrade(int token)
    ObjectReference chest = GetVendorChest(token)
    Actor follower = GetTradeFollower(token)
    if chest == none || follower == none
        ReportTrade(token, 0, 0, -1)   ; native logs the abort + frees the token
        return
    endif

    ; The vendor's barter funds ARE the merchant chest's Gold001 (not the actor's
    ; pocket) -- the crash-free GetItemCount path C.O.I.N. proves.
    Form gold001 = Game.GetFormFromFile(0x0000000F, "Skyrim.esm")
    int vendorGold = chest.GetItemCount(gold001)
    bool probe = GetProbeOnly(token)

    Form[] forms = GetSellForms(token)
    Int[]  counts = GetSellCounts(token)
    Int[]  values = GetSellValues(token)

    ; SELL loop -- native pre-sorted highest-value first; cap at the chest's gold.
    int soldValue = 0
    int soldCount = 0
    int budget = vendorGold
    int i = 0
    int n = forms.Length
    while i < n
        Form f = forms[i]
        int unit = values[i]
        int have = counts[i]
        if f && unit > 0 && have > 0 && unit <= budget
            int canBuy = budget / unit
            if canBuy > have
                canBuy = have
            endif
            if canBuy > 0
                if !probe
                    follower.RemoveItem(f, canBuy, true, chest)      ; goods -> vendor
                    follower.AddItem(gold001, unit * canBuy, true)   ; follower gets paid
                    chest.RemoveItem(gold001, unit * canBuy, true)   ; vendor pays from its barter gold
                endif
                soldValue += unit * canBuy
                soldCount += canBuy
                budget -= unit * canBuy
            endif
        endif
        i += 1
    endwhile

    ReportTrade(token, soldValue, soldCount, vendorGold)
EndFunction
