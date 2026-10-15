# End-to-end check against the real clipboard. Needs an interactive desktop and Windows PowerShell 5.1.
# Usage (after a Release build):  powershell -File tests\e2e.ps1
# Runs in portable mode next to the exe, so your real history is never touched. Your clipboard text is restored.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
'@
$exe = Resolve-Path (Join-Path $PSScriptRoot '..\build\Release\clipjournal.exe')
$dir = Split-Path $exe
$items = Join-Path $dir 'items'
if (Get-Process clipjournal -ErrorAction SilentlyContinue) { throw 'Close the running clipjournal first (single instance).' }

function Check($cond, $what) { if (-not $cond) { throw "FAIL: $what" }; "ok    $what" }
function Wait { Start-Sleep -Milliseconds 400 }

$saved = Get-Clipboard -Raw
$tmp = Join-Path $env:TEMP 'clipjournal-e2e.txt'
'x' | Set-Content $tmp
"[clipjournal]`r`nhotkey=Ctrl+Alt+F11`r`nignore_apps=" | Set-Content (Join-Path $dir 'config.ini') -Encoding ASCII
$p = Start-Process $exe -PassThru
Start-Sleep -Milliseconds 800
try {
  Set-Clipboard -Value 'hello clipjournal'; Wait
  Set-Clipboard -Value 'https://example.com/path?q=1'; Wait
  [System.Windows.Forms.Clipboard]::SetImage((New-Object System.Drawing.Bitmap 320, 200)); Wait
  Set-Clipboard -Path $tmp; Wait
  $do = New-Object System.Windows.Forms.DataObject  # how password managers mark secrets
  $do.SetText('SECRET-PASSWORD')
  $do.SetData('ExcludeClipboardContentFromMonitorProcessing', (New-Object System.IO.MemoryStream (,[byte[]](0))))
  [System.Windows.Forms.Clipboard]::SetDataObject($do, $true); Wait
  Set-Clipboard -Value 'hello clipjournal'; Wait

  $files = Get-ChildItem $items
  Check ($files.Count -eq 4) 'text, link, image and file stored; repeated copy not stored twice'
  Check (@($files | Where-Object Extension -eq '.png').Count -eq 1) 'image stored as PNG'
  Check (-not (Get-ChildItem $items -Filter *.txt | Where-Object { (Get-Content $_.FullName -Raw) -match 'SECRET' })) 'secret skipped'

  [System.Windows.Forms.SendKeys]::SendWait('^%{F11}'); Start-Sleep -Milliseconds 600
  $h = [W]::FindWindow('clipjournal', [NullString]::Value)  # $null would turn into ""
  Check ([W]::GetForegroundWindow() -eq $h) 'hotkey opens the popup with focus'  # throws before any key can hit another app
  [System.Windows.Forms.SendKeys]::SendWait('{DOWN}+{ENTER}'); Wait  # 2nd row = the file; Shift+Enter = copy only
  Check (-not [W]::IsWindowVisible($h)) 'popup closes after pick'
  Check (@(Get-Clipboard -Format FileDropList | ForEach-Object { "$_" }) -contains $tmp) 'picked file is back on the clipboard'
  Check ((Get-ChildItem $items).Count -eq 4) 'own clipboard write is not recorded again'
} finally {
  Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
  Remove-Item $items, (Join-Path $dir 'config.ini'), $tmp -Recurse -Force -ErrorAction SilentlyContinue
  if ($saved) { Set-Clipboard -Value $saved }
}
