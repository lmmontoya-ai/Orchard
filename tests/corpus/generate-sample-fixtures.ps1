param()

$samplesDir = Join-Path $PSScriptRoot "samples"

$ApfsBlockSize = 4096
$BtreeInfoSize = 40
$OmapObjectBlock = [UInt64]2
$OmapRootBlock = [UInt64]3
$OmapLeafBlock = [UInt64]4
$LegacyVolumeBlock = [UInt64]5
$CurrentVolumeBlock = [UInt64]6
$VolumeObjectId = [UInt64]77
$CurrentCheckpointXid = [UInt64]42
$OmapValueDeleted = [UInt32]1

function Write-Le16 {
  param(
    [byte[]]$Bytes,
    [int]$Offset,
    [UInt16]$Value
  )

  $Bytes[$Offset] = [byte]($Value -band 0xFF)
  $Bytes[$Offset + 1] = [byte](($Value -shr 8) -band 0xFF)
}

function Write-Le32 {
  param(
    [byte[]]$Bytes,
    [int]$Offset,
    [UInt32]$Value
  )

  Write-Le16 -Bytes $Bytes -Offset $Offset -Value ([UInt16]($Value -band 0xFFFF))
  Write-Le16 -Bytes $Bytes -Offset ($Offset + 2) -Value ([UInt16](($Value -shr 16) -band 0xFFFF))
}

function Write-Le64 {
  param(
    [byte[]]$Bytes,
    [int]$Offset,
    [UInt64]$Value
  )

  Write-Le32 -Bytes $Bytes -Offset $Offset -Value ([UInt32]($Value -band 0xFFFFFFFF))
  Write-Le32 -Bytes $Bytes -Offset ($Offset + 4) -Value ([UInt32](($Value -shr 32) -band 0xFFFFFFFF))
}

function Write-Ascii {
  param(
    [byte[]]$Bytes,
    [int]$Offset,
    [string]$Text
  )

  $textBytes = [System.Text.Encoding]::ASCII.GetBytes($Text)
  [Array]::Copy($textBytes, 0, $Bytes, $Offset, $textBytes.Length)
}

function Write-Utf16Le {
  param(
    [byte[]]$Bytes,
    [int]$Offset,
    [string]$Text
  )

  $textBytes = [System.Text.Encoding]::Unicode.GetBytes($Text)
  [Array]::Copy($textBytes, 0, $Bytes, $Offset, $textBytes.Length)
}

function Write-Bytes {
  param(
    [byte[]]$Target,
    [int]$Offset,
    [byte[]]$Source
  )

  [Array]::Copy($Source, 0, $Target, $Offset, $Source.Length)
}

function Write-ObjectHeader {
  param(
    [byte[]]$Bytes,
    [int]$BaseOffset,
    [UInt64]$Oid,
    [UInt64]$Xid,
    [UInt32]$Type,
    [UInt32]$Subtype
  )

  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x00) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x08) -Value $Oid
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x10) -Value $Xid
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x18) -Value $Type
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x1C) -Value $Subtype
}

function Write-NxSuperblock {
  param(
    [byte[]]$Bytes,
    [int]$BaseOffset,
    [UInt64]$Xid,
    [UInt64]$BlockCount
  )

  Write-ObjectHeader -Bytes $Bytes -BaseOffset $BaseOffset -Oid ([UInt64]($BaseOffset / $ApfsBlockSize)) -Xid $Xid -Type 1 -Subtype 0
  Write-Ascii -Bytes $Bytes -Offset ($BaseOffset + 0x20) -Text "NXSB"
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x24) -Value $ApfsBlockSize
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x28) -Value $BlockCount
  Write-Bytes -Target $Bytes -Offset ($BaseOffset + 0x48) -Source ([byte[]](0x10,0x11,0x12,0x13,0x20,0x21,0x22,0x23,0x30,0x31,0x32,0x33,0x40,0x41,0x42,0x43))
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x60) -Value ($Xid + 1)
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x68) -Value 1
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x70) -Value 1
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x98) -Value 5
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0xA0) -Value $OmapObjectBlock
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0xA8) -Value 7
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0xB4) -Value 100
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0xB8) -Value $VolumeObjectId
}

function Write-OmapSuperblock {
  param(
    [byte[]]$Bytes,
    [int]$BaseOffset
  )

  Write-ObjectHeader -Bytes $Bytes -BaseOffset $BaseOffset -Oid $OmapObjectBlock -Xid $CurrentCheckpointXid -Type 0x0B -Subtype 0x0B
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x20) -Value 0
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x24) -Value 0
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x28) -Value 0x0B
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x2C) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x30) -Value $OmapRootBlock
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x38) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x40) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x48) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x50) -Value 0
}

function Write-BtreeInfoFooter {
  param(
    [byte[]]$Bytes,
    [int]$BaseOffset,
    [UInt32]$KeySize,
    [UInt32]$ValueSize,
    [UInt64]$KeyCount,
    [UInt64]$NodeCount
  )

  $footerOffset = $BaseOffset + $ApfsBlockSize - $BtreeInfoSize
  Write-Le32 -Bytes $Bytes -Offset ($footerOffset + 0x00) -Value 0
  Write-Le32 -Bytes $Bytes -Offset ($footerOffset + 0x04) -Value $ApfsBlockSize
  Write-Le32 -Bytes $Bytes -Offset ($footerOffset + 0x08) -Value $KeySize
  Write-Le32 -Bytes $Bytes -Offset ($footerOffset + 0x0C) -Value $ValueSize
  Write-Le32 -Bytes $Bytes -Offset ($footerOffset + 0x10) -Value $KeySize
  Write-Le32 -Bytes $Bytes -Offset ($footerOffset + 0x14) -Value $ValueSize
  Write-Le64 -Bytes $Bytes -Offset ($footerOffset + 0x18) -Value $KeyCount
  Write-Le64 -Bytes $Bytes -Offset ($footerOffset + 0x20) -Value $NodeCount
}

function Write-OmapKey {
  param(
    [byte[]]$Bytes,
    [int]$Offset,
    [UInt64]$Oid,
    [UInt64]$Xid
  )

  Write-Le64 -Bytes $Bytes -Offset ($Offset + 0x00) -Value $Oid
  Write-Le64 -Bytes $Bytes -Offset ($Offset + 0x08) -Value $Xid
}

function Write-OmapValue {
  param(
    [byte[]]$Bytes,
    [int]$Offset,
    [UInt32]$Flags,
    [UInt64]$PhysicalBlock
  )

  Write-Le32 -Bytes $Bytes -Offset ($Offset + 0x00) -Value $Flags
  Write-Le32 -Bytes $Bytes -Offset ($Offset + 0x04) -Value $ApfsBlockSize
  Write-Le64 -Bytes $Bytes -Offset ($Offset + 0x08) -Value $PhysicalBlock
}

function Write-OmapRootNode {
  param(
    [byte[]]$Bytes,
    [int]$BaseOffset
  )

  $tableLength = 4
  $keyAreaStart = $BaseOffset + 0x38 + $tableLength
  $valueAreaEnd = $BaseOffset + $ApfsBlockSize - $BtreeInfoSize
  $valueOffset = $valueAreaEnd - 8

  Write-ObjectHeader -Bytes $Bytes -BaseOffset $BaseOffset -Oid $OmapRootBlock -Xid $CurrentCheckpointXid -Type 3 -Subtype 0x0B
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x20) -Value 0x0005
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x22) -Value 1
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x24) -Value 1
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x28) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x2A) -Value $tableLength
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x2C) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x2E) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x30) -Value 0xFFFF
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x32) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x34) -Value 0xFFFF
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x36) -Value 0

  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x38) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x3A) -Value ([UInt16](($BaseOffset + $ApfsBlockSize - $BtreeInfoSize) - $valueOffset))
  Write-OmapKey -Bytes $Bytes -Offset $keyAreaStart -Oid $VolumeObjectId -Xid 20
  Write-Le64 -Bytes $Bytes -Offset $valueOffset -Value $OmapLeafBlock
  Write-BtreeInfoFooter -Bytes $Bytes -BaseOffset $BaseOffset -KeySize 16 -ValueSize 8 -KeyCount 2 -NodeCount 2
}

function Write-OmapLeafNode {
  param(
    [byte[]]$Bytes,
    [int]$BaseOffset,
    [UInt32]$LatestFlags
  )

  $records = @(
    @{ Oid = $VolumeObjectId; Xid = [UInt64]20; Flags = [UInt32]0; PhysicalBlock = $LegacyVolumeBlock },
    @{ Oid = $VolumeObjectId; Xid = $CurrentCheckpointXid; Flags = $LatestFlags; PhysicalBlock = $CurrentVolumeBlock }
  )

  $tableLength = [UInt16]($records.Count * 4)
  $keyAreaStart = $BaseOffset + 0x38 + $tableLength
  $valueAreaEnd = $BaseOffset + $ApfsBlockSize

  Write-ObjectHeader -Bytes $Bytes -BaseOffset $BaseOffset -Oid $OmapLeafBlock -Xid $CurrentCheckpointXid -Type 3 -Subtype 0x0B
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x20) -Value 0x0006
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x22) -Value 0
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x24) -Value $records.Count
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x28) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x2A) -Value $tableLength
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x2C) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x2E) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x30) -Value 0xFFFF
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x32) -Value 0
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x34) -Value 0xFFFF
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x36) -Value 0

  for ($index = 0; $index -lt $records.Count; $index++) {
    $tocOffset = $BaseOffset + 0x38 + ($index * 4)
    $keyOffset = [UInt16]($index * 16)
    $valueOffset = [UInt16](($index + 1) * 16)
    $keyWriteOffset = $keyAreaStart + $keyOffset
    $valueWriteOffset = $valueAreaEnd - $valueOffset

    Write-Le16 -Bytes $Bytes -Offset ($tocOffset + 0x00) -Value $keyOffset
    Write-Le16 -Bytes $Bytes -Offset ($tocOffset + 0x02) -Value $valueOffset
    Write-OmapKey -Bytes $Bytes -Offset $keyWriteOffset -Oid $records[$index].Oid -Xid $records[$index].Xid
    Write-OmapValue -Bytes $Bytes -Offset $valueWriteOffset -Flags $records[$index].Flags -PhysicalBlock $records[$index].PhysicalBlock
  }
}

function Write-VolumeSuperblock {
  param(
    [byte[]]$Bytes,
    [int]$BaseOffset,
    [UInt64]$Xid,
    [UInt64]$IncompatibleFeatures,
    [UInt16]$Role,
    [string]$Name
  )

  Write-ObjectHeader -Bytes $Bytes -BaseOffset $BaseOffset -Oid $VolumeObjectId -Xid $Xid -Type 0x0D -Subtype 0
  Write-Ascii -Bytes $Bytes -Offset ($BaseOffset + 0x20) -Text "APSB"
  Write-Le32 -Bytes $Bytes -Offset ($BaseOffset + 0x24) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x28) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x30) -Value 0
  Write-Le64 -Bytes $Bytes -Offset ($BaseOffset + 0x38) -Value $IncompatibleFeatures
  Write-Bytes -Target $Bytes -Offset ($BaseOffset + 0xF0) -Source ([byte[]](0x50,0x51,0x52,0x53,0x60,0x61,0x62,0x63,0x70,0x71,0x72,0x73,0x80,0x81,0x82,0x83))
  Write-Ascii -Bytes $Bytes -Offset ($BaseOffset + 0x2C0) -Text $Name
  Write-Le16 -Bytes $Bytes -Offset ($BaseOffset + 0x3C4) -Value $Role
}

function New-DirectFixture {
  param(
    [string]$Path,
    [string]$VolumeName,
    [UInt64]$IncompatibleFeatures,
    [UInt16]$Role,
    [UInt32]$LatestFlags = 0
  )

  $bytes = New-Object byte[] ($ApfsBlockSize * 8)
  Write-NxSuperblock -Bytes $bytes -BaseOffset 0 -Xid 1 -BlockCount 8
  Write-NxSuperblock -Bytes $bytes -BaseOffset $ApfsBlockSize -Xid $CurrentCheckpointXid -BlockCount 8
  Write-OmapSuperblock -Bytes $bytes -BaseOffset ($ApfsBlockSize * [int]$OmapObjectBlock)
  Write-OmapRootNode -Bytes $bytes -BaseOffset ($ApfsBlockSize * [int]$OmapRootBlock)
  Write-OmapLeafNode -Bytes $bytes -BaseOffset ($ApfsBlockSize * [int]$OmapLeafBlock) -LatestFlags $LatestFlags
  Write-VolumeSuperblock -Bytes $bytes -BaseOffset ($ApfsBlockSize * [int]$LegacyVolumeBlock) -Xid 20 -IncompatibleFeatures 1 -Role 0x0040 -Name "Legacy Data"
  Write-VolumeSuperblock -Bytes $bytes -BaseOffset ($ApfsBlockSize * [int]$CurrentVolumeBlock) -Xid $CurrentCheckpointXid -IncompatibleFeatures $IncompatibleFeatures -Role $Role -Name $VolumeName
  [System.IO.File]::WriteAllBytes($Path, $bytes)
}

function New-GptFixture {
  param([string]$Path)

  $logicalBlockSize = 512
  $bytes = New-Object byte[] ($logicalBlockSize * 256)
  $firstLba = [UInt64]40
  $lastLba = [UInt64]103

  Write-Ascii -Bytes $bytes -Offset $logicalBlockSize -Text "EFI PART"
  Write-Le32 -Bytes $bytes -Offset ($logicalBlockSize + 8) -Value 0x00010000
  Write-Le32 -Bytes $bytes -Offset ($logicalBlockSize + 12) -Value 92
  Write-Le64 -Bytes $bytes -Offset ($logicalBlockSize + 24) -Value 1
  Write-Le64 -Bytes $bytes -Offset ($logicalBlockSize + 32) -Value 255
  Write-Le64 -Bytes $bytes -Offset ($logicalBlockSize + 40) -Value 34
  Write-Le64 -Bytes $bytes -Offset ($logicalBlockSize + 48) -Value 200
  Write-Le64 -Bytes $bytes -Offset ($logicalBlockSize + 72) -Value 2
  Write-Le32 -Bytes $bytes -Offset ($logicalBlockSize + 80) -Value 1
  Write-Le32 -Bytes $bytes -Offset ($logicalBlockSize + 84) -Value 128

  $partitionOffset = $logicalBlockSize * 2
  Write-Bytes -Target $bytes -Offset $partitionOffset -Source ([byte[]](0xEF,0x57,0x34,0x7C,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC))
  Write-Bytes -Target $bytes -Offset ($partitionOffset + 16) -Source ([byte[]](0x91,0x92,0x93,0x94,0xA0,0xA1,0xA2,0xA3,0xB0,0xB1,0xB2,0xB3,0xC0,0xC1,0xC2,0xC3))
  Write-Le64 -Bytes $bytes -Offset ($partitionOffset + 32) -Value $firstLba
  Write-Le64 -Bytes $bytes -Offset ($partitionOffset + 40) -Value $lastLba
  Write-Utf16Le -Bytes $bytes -Offset ($partitionOffset + 56) -Text "Orchard GPT"

  $containerOffset = [int]($firstLba * $logicalBlockSize)
  $containerPath = Join-Path $samplesDir "_tmp_direct.img"
  New-DirectFixture -Path $containerPath -VolumeName "GPT Data" -IncompatibleFeatures 1 -Role 0x0040
  $containerBytes = [System.IO.File]::ReadAllBytes($containerPath)
  [Array]::Copy($containerBytes, 0, $bytes, $containerOffset, $containerBytes.Length)
  Remove-Item -LiteralPath $containerPath -Force

  [System.IO.File]::WriteAllBytes($Path, $bytes)
}

New-Item -ItemType Directory -Force -Path $samplesDir | Out-Null

New-DirectFixture -Path (Join-Path $samplesDir "plain-user-data.img") -VolumeName "Orchard Data" -IncompatibleFeatures 1 -Role 0x0040
New-GptFixture -Path (Join-Path $samplesDir "gpt-user-data.img")
New-DirectFixture -Path (Join-Path $samplesDir "snapshot-volume.img") -VolumeName "Snapshot Data" -IncompatibleFeatures 3 -Role 0x0040
New-DirectFixture -Path (Join-Path $samplesDir "sealed-system.img") -VolumeName "System" -IncompatibleFeatures 0x21 -Role 0x0001
