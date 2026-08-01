Scriptname MFO_Trade extends Quest
{#21 econ bridge (Fable's ECON_PAPYRUS_PLAN). The DLL owns the DECISION (vendor
 resolution, sellables, buy needs, catalog) and dispatches RunTrade(token) at this
 quest. This script does what native cannot without a CTD -- the merchant-safe
 read + transaction -- pulling its order from MFO-registered natives keyed by
 token, then reporting the result back.

 SELL: sell the follower's junk to the vendor, highest-value first, capped at the
 chest's barter gold. BUY: enumerate the vendor's ACTUAL stock (po3) and let native
 PlanBuy rank it (best affordable, up to the number needed, bounded by the purse),
 then buy the plan. bEconomy off = a dry run: it still reads + enumerates + plans
 (so the reads are exercised) but mutates nothing.}

; ── MFO-registered natives (return SYNCHRONOUSLY to the calling Papyrus frame). ─
Actor           Function GetTradeFollower(int token) global native
ObjectReference Function GetVendorChest(int token)   global native
Actor           Function GetVendorActor(int token)   global native
Bool            Function GetProbeOnly(int token)      global native
Form[]          Function GetSellForms(int token)      global native
Int[]           Function GetSellCounts(int token)     global native
Int[]           Function GetSellValues(int token)     global native
Int[]           Function PlanBuy(int token, Form[] forms, Int[] counts) global native
Int             Function GetBuySpent(int token)       global native
Function ReportTrade(int token, int soldValue, int soldCount, int boughtCount, int spent, int vendorGold) global native

Function RunTrade(int token)
    ObjectReference chest = GetVendorChest(token)
    Actor follower = GetTradeFollower(token)
    if chest == none || follower == none
        ReportTrade(token, 0, 0, 0, 0, -1)   ; native logs the abort + frees the token
        return
    endif

    ; The vendor's barter funds ARE the merchant chest's Gold001 (crash-free path).
    Form gold001 = Game.GetFormFromFile(0x0000000F, "Skyrim.esm")
    int vendorGold = chest.GetItemCount(gold001)
    bool probe = GetProbeOnly(token)

    ; ── SELL: native pre-sorted highest-value first; cap at the chest's gold. ──
    Form[] forms = GetSellForms(token)
    Int[]  counts = GetSellCounts(token)
    Int[]  values = GetSellValues(token)
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
                    follower.RemoveItem(f, canBuy, true, chest)
                    follower.AddItem(gold001, unit * canBuy, true)
                    chest.RemoveItem(gold001, unit * canBuy, true)
                endif
                soldValue += unit * canBuy
                soldCount += canBuy
                budget -= unit * canBuy
            endif
        endif
        i += 1
    endwhile

    ; ── BUY: enumerate the vendor's ACTUAL stock, native plans it, execute. ──
    int boughtCount = 0
    int spent = 0
    Form[] stock = PO3_SKSEFunctions.AddAllItemsToArray(chest, false, false, true)
    int m = stock.Length
    if m > 0
        int[] stockCounts = new int[128]
        int a = 0
        while a < m && a < 128
            stockCounts[a] = chest.GetItemCount(stock[a])
            a += 1
        endwhile
        int[] plan = PlanBuy(token, stock, stockCounts)
        int b = 0
        while b < m && b < plan.Length
            if plan[b] > 0
                boughtCount += plan[b]
                if !probe
                    chest.RemoveItem(stock[b], plan[b], true, follower)   ; goods -> follower
                endif
            endif
            b += 1
        endwhile
        spent = GetBuySpent(token)
        if spent > 0 && !probe
            follower.RemoveItem(gold001, spent, true, chest)   ; follower pays into the vendor's chest
        endif
    endif

    ReportTrade(token, soldValue, soldCount, boughtCount, spent, vendorGold)
EndFunction
