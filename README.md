# esp_tests

## Useful commands

```
winget install usbipd
```

```
./configure.sh <project-dir> <target>
```

```
idf.py -B <target> build flash monitor
```

## Troubleshooting

### LIBUSB_ERROR_ACCESS

1. enable systemd/udev in WSL

```
echo -e '[boot]\nsystemd=true' | sudo tee /etc/wsl.conf
```

```
wsl.exe --shutdown
```

2. drop the OpenOCD rule set in place

```
sudo cp $OPENOCD_SCRIPTS/contrib/60-openocd.rules /etc/udev/rules.d/
```

3. add an explicit rule for the ESP USB-JTAG (VID 303a, PID 1001)

```
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="303a", ATTR{idProduct}=="1001", \
      MODE="0660", GROUP="plugdev"' | sudo tee /etc/udev/rules.d/99-espressif-jtag.rules
```

4. reload rules

```
sudo udevadm control --reload-rules && sudo udevadm trigger
```