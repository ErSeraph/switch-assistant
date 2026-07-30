param(
    [string] $InputPath = "romfs\titles\titles.txt",
    [string] $OutputPath = "romfs\titles\titles.idx"
)

$inputFullPath = [System.IO.Path]::GetFullPath($InputPath)
$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
$bytes = [System.IO.File]::ReadAllBytes($inputFullPath)
$records = @{}
$offset = 0

while ($offset -lt $bytes.Length) {
    $lineOffset = $offset
    while ($offset -lt $bytes.Length -and $bytes[$offset] -ne 10) {
        $offset++
    }

    $lineEnd = $offset
    if ($lineEnd -gt $lineOffset -and $bytes[$lineEnd - 1] -eq 13) {
        $lineEnd--
    }

    $lineLength = $lineEnd - $lineOffset
    if ($lineLength -ge 17) {
        $sep = -1
        for ($i = $lineOffset; $i -lt $lineEnd; $i++) {
            if ($bytes[$i] -eq 59) {
                $sep = $i
                break
            }
        }

        if ($sep -gt $lineOffset) {
            $key = [System.Text.Encoding]::ASCII.GetString($bytes, $lineOffset, $sep - $lineOffset).Trim()
            $valueLength = $lineEnd - $sep - 1
            if ($key.Length -gt 0 -and $valueLength -gt 0) {
                try {
                    $titleId = [Convert]::ToUInt64(($key -replace '^0[xX]', ''), 16)
                    if ($titleId -ne 0 -and -not $records.ContainsKey($titleId)) {
                        $records[$titleId] = [uint64] $lineOffset
                    }
                } catch {
                }
            }
        }
    }

    if ($offset -lt $bytes.Length -and $bytes[$offset] -eq 10) {
        $offset++
    }
}

$outputDir = [System.IO.Path]::GetDirectoryName($outputFullPath)
if ($outputDir) {
    [System.IO.Directory]::CreateDirectory($outputDir) | Out-Null
}

$stream = [System.IO.File]::Create($outputFullPath)
$writer = New-Object System.IO.BinaryWriter -ArgumentList $stream
try {
    $writer.Write([System.Text.Encoding]::ASCII.GetBytes("SHAIDX1`0"))
    $writer.Write([uint32] 1)
    $writer.Write([uint32] $records.Count)

    foreach ($titleId in ($records.Keys | Sort-Object)) {
        $writer.Write([uint64] $titleId)
        $writer.Write([uint64] $records[$titleId])
    }
} finally {
    $writer.Dispose()
}

Write-Host "Wrote $($records.Count) index records to $outputFullPath"
