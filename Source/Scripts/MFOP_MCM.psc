Scriptname MFOP_MCM extends MCM_ConfigBase
; MFO Progression Add-On — MCM registration shim (the addon's OWN MCM tab).
;
; Deliberately empty. MCM Helper renders every control from
; Data/MCM/Config/MFO_Progression/config.json. The economy sliders are bound
; (GlobalValue) directly to this ESL's MFOP_* economy globals, so moving a
; slider writes the global; MFO's DLL re-reads the manifest economy on menu
; close (generically — it never names this addon). This subclass exists only
; so MCM Helper has a concrete MCM_ConfigBase-derived quest script to register.
;
; This file ships INSIDE the addon (as if a third party authored it); MFO.esp
; and MFO.dll have no reference to it.
