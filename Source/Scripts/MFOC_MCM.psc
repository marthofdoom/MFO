Scriptname MFOC_MCM extends MCM_ConfigBase
; MFO Class Add-On: MCM registration shim (the add-on's OWN MCM tab).
;
; Deliberately empty. MCM Helper renders every control from
; Data/MCM/Config/MFO_Classes/config.json; the controls are ModSetting* values
; that MCM Helper writes to Data/MCM/Settings/MFO_Classes.ini, which MFO.dll
; reads (Config::Read) on load and on every MCM close. This subclass exists only
; so MCM Helper has a concrete MCM_ConfigBase-derived quest script to register.
;
; This file ships INSIDE the add-on (as if a third party authored it); MFO.esp
; has no reference to it.
