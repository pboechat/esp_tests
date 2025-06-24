# Elevate to Administrator if not already
if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {

    try {
        Start-Process powershell -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
        exit
    } catch {
        Write-Error "Failed to elevate to administrator. Exiting."
		Write-Host "Press enter to continue..."
		Read-Host > $null
        exit 1
    }
}

# Run usbipd list and capture output
try {
	$usbipdOutput = usbipd list 2>&1
} catch {
	Write-Error "Failed to run 'usbipd list'. Is usbipd installed and in PATH?"
	Write-Host "Press enter to continue..."
	Read-Host > $null
	exit 1
}

$usbDevices = 'Silicon Labs CP210x USB to UART Bridge','USB-SERIAL CH340K','USB JTAG/serial debug unit'

# BUSID =   digits "-" digits [("." digits)...]   followed by ≥2 spaces
$busIdRegex = '^(\d+-\d+(?:\.\d+)*)\s{2,}'

$connected = 0

foreach ($usbDevice in $usbDevices) {
	Write-Host "Looking for USB device '$usbDevice'..." -ForegroundColor Cyan

	# Search for the matching line
	$line = [string]($usbipdOutput | Where-Object { $_ -match "$usbDevice" })

	if (-not $line) {
		Write-Error "Device not found. Make sure it's plugged in and 'usbipd list' shows it."
		continue
	}

	if ($line -match $busIdRegex) {
		$busid = $matches[1]
	} else {
		Write-Error "Device not connected. Make sure it's plugged in."
		continue
	}

	Write-Host "Found device with BusID: $busid" -ForegroundColor Green

	# Attempt to bind the device
	Write-Host "Binding device..." -ForegroundColor Cyan
	$bindResult = usbipd bind --busid $busid 2>&1
	if ($lastexitcode -ne 0) {
		Write-Error "Failed to bind device. Output: $bindResult"
		continue
	}

	# Attempt to attach to WSL
	Write-Host "Attaching device to WSL..." -ForegroundColor Cyan
	$attachResult = usbipd attach --busid $busid --wsl 2>&1
	if ($lastexitcode -ne 0) {
		if ($attachResult -like '*already attached*') {
			Write-Host "Device already attached." -ForegroundColor Gray
			$attached = 1
			continue
		} else {
			Write-Error "Failed to attach device to WSL. Output: $attachResult"
			continue
		}
	}

	Write-Host "Successfully attached $usbDevice (BusID: $busid) to WSL." -ForegroundColor Cyan
	$attached = 1
}

if ($attached -eq 1) {
	Write-Host "Press enter to continue..."
	Read-Host > $null
	exit 0
} else {
	Write-Error "Failed to attach any device."
	Write-Host "Press enter to continue..."
	Read-Host > $null
	exit 1
}