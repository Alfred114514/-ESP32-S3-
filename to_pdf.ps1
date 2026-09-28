$ErrorActionPreference = 'Stop'
$src = 'E:\0cubemx-project\历年题目\项目深挖\李博宇.docx'
$outDir = 'D:\cubemx\esp32-s3\esp32_video\esp32_cam_video'
$pdf = Join-Path $outDir '李博宇-当前版.pdf'
$word = New-Object -ComObject Word.Application
try {
    $word.Visible = $false
    $doc = $word.Documents.Open($src, $false, $true)
    $doc.SaveAs2($pdf, 17)
    $pages = $doc.ComputeStatistics(2)
    Write-Output ("PAGES=" + $pages)
    $words = $doc.ComputeStatistics(0)
    Write-Output ("WORDS=" + $words)
    $chars = $doc.ComputeStatistics(3)
    Write-Output ("CHARS=" + $chars)
    $lines = $doc.ComputeStatistics(1)
    Write-Output ("LINES=" + $lines)
    $doc.Close($false)
} finally {
    $word.Quit()
}
Write-Output ("PDF=" + $pdf)
