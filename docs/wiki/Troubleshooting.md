# Troubleshooting

## The installer cannot find the controller over USB

- Use **Chrome or Edge** on a computer. Other browsers cannot talk to USB devices.
- Only one program can use the USB port: close the **Arduino IDE Serial Monitor**, PlatformIO's monitor or another installer tab.
- A **new or empty** controller has no USB port in the list: it shows up as a drive called **RP2350**. See [[Install]].
- Windows shows **"Unknown USB Device (Device Descriptor Request Failed)"**: the cable or socket is not making a good data connection. If it happens even with BOOT held (install mode runs from the chip's built-in ROM, so no firmware can cause it), try **another USB-C data cable**, plugged straight into the computer, not through a hub. Reseating the cable often fixes it for a while.

## I lost the controller's IP address

- Connect over USB on the Install page: step 3 and the install log show the address.
- Hold the **USER** button while powering up: the controller starts on **192.168.4.1/24** for a laptop plugged in directly.

## http://<IP>/ does not open

- The web page needs firmware **0.3.2 or newer**. Check the version in the installer and click **Install**.
- Check the address in the install log, and that your computer is on the same network.

## "Save to controller" or "Update from GitHub" says no password

No web password is saved yet. Set one under **Connections → Web page** and send the settings **over USB once** with **Include the passwords** ticked. See [[Controller web page|Controller-Web-Page]].

## MQTT does not connect

- The overview and the install log show `MQTT: not connected to …`.
- Check the broker address and port, and that the broker allows the user (or anonymous logins).
- Port 8883: turn on **Encrypt with TLS**.
- With a CA certificate: the controller's clock must be right (connect the installer over USB once), and the broker certificate must list the broker address as a **DNS name**. See [[MQTT]].

## An analog input shows "Wire break"

The input is in **4–20 mA** mode and reads below 3.6 mA: a broken wire, a failed sensor or nothing connected. Above 21 mA it shows **Over range**. In 0–10 V mode nothing connected reads about 0 V. The **AN-IN MODE** DIP switch must match the mode you choose.

## An RS485 point shows NaN or "no reply"

Use **Test read** on the point (see [[RS485]]): it says whether the sensor answers and why not. Check slave ID, wiring (A, B, G), baud rate, parity and termination.

## The clock says "lost"

The clock stopped while the controller was unpowered: the CR2032 is flat or missing, or the time was never set. Connecting the installer over USB sets it from your computer.

## After an update or new settings the outputs are off

That is on purpose: DO0–DO3 start off after every restart. SCADA or MQTT switch them on again.
