# ⏰ BLARE — The Game-Powered Alarm Clock

> **"Imagine having to play a random game just to snooze your clock! Well… you don't have to imagine that anymore."**

**BLARE** is an alarm clock engineered to eliminate the habit of just slapping the snooze button. Powered by the **Seeed Studio XIAO ESP32-C3**, BLARE forces your brain to wake up by requiring you to complete mini-games using tactile arcade-style controls before allowing you to silence or snooze the alarm.

---

## 📸 Overview & Design

BLARE combines a custom-designed PCB, a multi-button interface, an OLED/LCD display breakout, and custom micro-game firmware.

```text
                    +-----------------------+
                    |    0.96" / 1.3" OLED  |
                    |    Display Header     |
                    +-----------+-----------+
                                |
 +---------------+  +-----------+-----------+  +---------------+
 |  Mechanical   |--|  XIAO ESP32-C3 Board  |--| Active Buzzer |
 |  Switches (4) |  |   (Core Controller)   |  | (Audio Alarm) |
 +---------------+  +-----------------------+  +---------------+
