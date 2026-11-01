# Applies the payphone sound to every WAV in voice\raw and writes it, named by its text key,
# into the game's gta2dx9_voice folder. Run it through phone_batch.bat.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$raw  = Join-Path $here 'raw'
$out  = 'G:\Grand Theft Auto 2\gta2dx9_voice'
$map  = Join-Path $here 'voice_keys.txt'

$ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ff) {
    Write-Host 'ffmpeg was not found. Install it once with:  winget install Gyan.FFmpeg   (then open a new window and run this again)' -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $raw)) { New-Item -ItemType Directory -Path $raw | Out-Null }
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }

# voice_keys.txt: one "<Magnific id> <text key>" per line, e.g.  dtUAzJnXSL 1132
$keys = @{}
if (Test-Path $map) {
    Get-Content $map | ForEach-Object {
        if ($_ -match '^\s*([A-Za-z0-9]+)\s+(\d{3,8})\s*$') { $keys[$Matches[1]] = $Matches[2] }
    }
}

# The payphone: the same curve as the Audition preset "Telephone - Voice Mail LOUD" (FFT Filter),
# read off its graph, then levelled to about -19.5 dB RMS with the peaks softly limited, which is
# where the first 20 lines sit. Tune the gain_entry numbers (Hz, dB) here.
$curve = 'entry(15,-90);entry(171,-90);entry(318,-78);entry(385,-27);entry(448,-9);entry(510,1.8);entry(659,6.7);entry(1054,7.5);entry(1638,4.5);entry(2512,-2.4);entry(3076,-24);entry(4390,-72);entry(6950,-90);entry(11400,-90)'
$chain = "firequalizer=gain_entry='$curve':scale=linlog:zero_phase=on:accuracy=1,loudnorm=I=-22:TP=-1:LRA=7"

$done = 0; $skipped = @()
Get-ChildItem $raw -Filter *.wav | ForEach-Object {
    $id = ($_.BaseName -split '_')[-1]
    $key = $keys[$id]
    if (-not $key -and $_.BaseName -match '^\d{3,8}$') { $key = $_.BaseName }   # already named by key
    if (-not $key) { $skipped += $_.Name; return }
    & $ff -hide_banner -loglevel error -y -i $_.FullName -af $chain -ac 1 -ar 24000 -c:a pcm_s16le (Join-Path $out "$key.wav")
    if ($LASTEXITCODE -ne 0) { Write-Host "failed: $($_.Name)" -ForegroundColor Red } else { $done++ ; Write-Host "$($_.Name) -> $key.wav" }
}
Write-Host "`n$done file(s) written to $out"
if ($skipped.Count) {
    Write-Host "No key known for these (add their Magnific id and key to voice_keys.txt):" -ForegroundColor Yellow
    $skipped | ForEach-Object { Write-Host "  $_" }
}
