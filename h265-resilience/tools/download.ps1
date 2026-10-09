param([string]$Destination = (Join-Path $PSScriptRoot '..\data'))
$ErrorActionPreference = 'Stop'
$destinationPath = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force $destinationPath | Out-Null
$sources = @(
    @{ Name='FourPeople'; Url='https://media.xiph.org/video/derf/webm/FourPeople_1280x720_60.webm' },
    @{ Name='KristenAndSara'; Url='https://media.xiph.org/video/derf/webm/KristenAndSara_1280x720_60.webm' }
)
foreach ($source in $sources) {
    $headers = & curl.exe --fail --silent --show-error --head --location --max-time 30 $source.Url
    if ($LASTEXITCODE -ne 0) { throw "Cannot get headers for $($source.Name)" }
    $lengthLine = $headers | Where-Object { $_ -match '^Content-Length:\s*(\d+)' } | Select-Object -Last 1
    if ($lengthLine -notmatch '^Content-Length:\s*(\d+)') { throw 'Missing Content-Length' }
    $length = [long]$Matches[1]
    $partDirectory = Join-Path $destinationPath ($source.Name + '_parts')
    New-Item -ItemType Directory -Force $partDirectory | Out-Null
    $chunk = 65536
    $count = [int][Math]::Ceiling($length / $chunk)
    $url = $source.Url
    0..($count - 1) | ForEach-Object -Parallel {
        $index = $_
        $begin = [long]$index * $using:chunk
        $end = [Math]::Min($begin + $using:chunk - 1, $using:length - 1)
        $part = Join-Path $using:partDirectory ('{0:D5}.part' -f $index)
        $expected = $end - $begin + 1
        if ((Test-Path -LiteralPath $part) -and (Get-Item -LiteralPath $part).Length -eq $expected) { return }
        & curl.exe --fail --silent --show-error --location --retry 3 --max-time 45 --range "$begin-$end" $using:url --output $part
        if ($LASTEXITCODE -ne 0 -or (Get-Item -LiteralPath $part).Length -ne $expected) {
            throw "Download/size verification failed: $part"
        }
    } -ThrottleLimit 12
    $output = Join-Path $destinationPath ($source.Name + '.webm')
    $stream = [IO.File]::Create($output)
    try {
        for ($index = 0; $index -lt $count; $index++) {
            $part = Join-Path $partDirectory ('{0:D5}.part' -f $index)
            $bytes = [IO.File]::ReadAllBytes($part)
            $stream.Write($bytes, 0, $bytes.Length)
        }
    } finally { $stream.Dispose() }
    if ((Get-Item -LiteralPath $output).Length -ne $length) { throw "Combined size mismatch: $output" }
    Get-FileHash -Algorithm SHA256 -LiteralPath $output
}
