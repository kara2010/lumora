// Verifiziert einen systemweiten Low-Level-Keyboard-Hook via koffi:
// Hook setzen -> Taste 'O' injizieren -> feuert der Callback mit vkCode=0x4F?
//   ./node_modules/.bin/electron _testlab/keyhook-test.js
const { app } = require('electron')

app.whenReady().then(() => {
  const out = { koffi: false, hookSet: false, callbackFired: false, vk: null, error: null }
  try {
    const koffi = require('koffi')
    out.koffi = 'v' + (koffi.version || '?')
    const u32 = koffi.load('user32.dll')
    const k32 = koffi.load('kernel32.dll')

    koffi.struct('KBDLLHOOKSTRUCT', { vkCode: 'uint32', scanCode: 'uint32', flags: 'uint32', time: 'uint32', dwExtraInfo: 'uintptr' })

    const SetWindowsHookExW = u32.func('void* SetWindowsHookExW(int idHook, void* lpfn, void* hMod, uint32 dwThreadId)')
    const UnhookWindowsHookEx = u32.func('int UnhookWindowsHookEx(void* hhk)')
    const CallNextHookEx = u32.func('intptr CallNextHookEx(void* hhk, int nCode, uintptr wParam, intptr lParam)')
    const GetModuleHandleW = k32.func('void* GetModuleHandleW(str16 name)')
    const keybd_event = u32.func('void keybd_event(uint8 bVk, uint8 bScan, uint32 dwFlags, uintptr dwExtraInfo)')

    // Callback-Prototyp: LRESULT CALLBACK LowLevelKeyboardProc(int, WPARAM, LPARAM)
    const HOOKPROC = koffi.proto('intptr __stdcall HookProc(int nCode, uintptr wParam, intptr lParam)')
    const jsProc = (nCode, wParam, lParam) => {
      try {
        if (nCode === 0 && (wParam === 256 /*WM_KEYDOWN*/ || wParam === 260 /*WM_SYSKEYDOWN*/)) {
          const ks = koffi.decode(lParam, 'KBDLLHOOKSTRUCT')
          out.callbackFired = true
          out.vk = ks.vkCode
        }
      } catch (e) { out.error = 'cb: ' + (e && e.message) }
      return CallNextHookEx(null, nCode, wParam, lParam)
    }
    const cb = koffi.register(jsProc, koffi.pointer(HOOKPROC))

    const hHook = SetWindowsHookExW(13 /*WH_KEYBOARD_LL*/, cb, GetModuleHandleW(null), 0)
    out.hookSet = !!hHook
    if (!hHook) throw new Error('SetWindowsHookEx fehlgeschlagen')

    // Taste 'O' (0x4F) injizieren
    setTimeout(() => { keybd_event(0x4F, 0, 0, 0); keybd_event(0x4F, 0, 2 /*KEYUP*/, 0) }, 300)
    setTimeout(() => {
      UnhookWindowsHookEx(hHook)
      console.log('KEYHOOK_TEST ' + JSON.stringify(out))
      app.quit()
    }, 900)
  } catch (e) {
    out.error = (e && e.message) + '\n' + (e && e.stack)
    console.log('KEYHOOK_TEST ' + JSON.stringify(out))
    app.quit()
  }
})
app.on('window-all-closed', () => app.quit())
