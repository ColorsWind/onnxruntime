$instruction = $args[0]
$original_jar_file_directory = $args[1]
$original_jar_file_name = $args[2]

$original_jar_file_full_path = "$original_jar_file_directory\$original_jar_file_name"
$extracted_file_directory = "$original_jar_file_directory\jar_extracted_full_files"

if ($instruction -eq "extract") {
    Write-Host "Extracting the jar file..."
    & 7z x $original_jar_file_full_path -o"$extracted_file_directory"
    Write-Host "Extracted files directory: $extracted_file_directory"

    Write-Host "Removing the original jar file..."
    Remove-Item -Path "$original_jar_file_full_path" -Force
    Write-Host "Removed the original jar file."
}
elseif ($instruction -eq "repack") {
    Write-Host "Removing ESRP's CodeSignSummary file..."
    Remove-Item -Path "$extracted_file_directory/CodeSignSummary*.md" -Force
    Write-Host "Removed ESRP's CodeSignSummary file."

    Write-Host "Repacking the jar file..."
    & 7z a "$original_jar_file_full_path" "$extracted_file_directory\*"
    Write-Host "Repacked the jar file."

    Write-Host "Removing the extracted files..."
    Remove-Item -Path "$extracted_file_directory" -Recurse -Force
    Write-Host "Removed the extracted files."
}
else {
    Write-Host "Invalid instruction: $instruction"
}
