// Headless-Test: laedt koffi + xinput1_4.dll in Electron und fragt die 4
// Controller-Slots ab. Kein Fenster; beendet sich selbst.
//   ./node_modules/.bin/electron _testlab/koffi-xinput-test.js
const { app } = require('electron')

app.whenReady().then(() => {
  const out = { koffi: false, dll: null, slots: [] }
  try {
    const koffi = require('koffi')
    out.koffi = 'geladen (version ' + (koffi.version || '?') + ')'

    let lib = null
    for (const name of ['xinput1_4.dll', 'xinput1_3.dll', 'xinput9_1_0.dll']) {
      try { lib = koffi.load(name); out.dll = name; break } catch (e) {}
    }
    if (!lib) throw new Error('keine XInput-DLL ladbar')

    koffi.struct('XINPUT_GAMEPAD', {
      wButtons: 'uint16', bLeftTrigger: 'uint8', bRightTrigger: 'uint8',
      sThumbLX: 'int16', sThumbLY: 'int16', sThumbRX: 'int16', sThumbRY: 'int16'
    })
    koffi.struct('XINPUT_STATE', { dwPacketNumber: 'uint32', Gamepad: 'XINPUT_GAMEPAD' })
    const XInputGetState = lib.func('uint32 __stdcall XInputGetState(uint32 dwUserIndex, _Out_ XINPUT_STATE *pState)')

    for (let i = 0; i < 4; i++) {
      const st = {}
      const ret = XInputGetState(i, st)
      out.slots.push({ slot: i, ret, connected: ret === 0, buttons: ret === 0 ? st.Gamepad.wButtons : null })
    }
    console.log('KOFFI_TEST_OK ' + JSON.stringify(out))
  } catch (e) {
    console.log('KOFFI_TEST_FAIL ' + (e && e.message))
  }
  app.quit()
})

app.on('window-all-closed', () => app.quit())
