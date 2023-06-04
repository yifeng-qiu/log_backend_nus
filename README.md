# Log Backend using Nordic Uart Service

Logging is an indispensible tool for firmware development. In particular, when debugging a BT stack, only single step breakpoints are usable as resuming stopped code execution causes kernel oops in Zephyr.

There are at least three use cases.

- If the UART is used for protocol communication with another chip, using it as log backend can be tricky as it requires the other device to ignore log outputs.
- When debugging BT devices getting power from a mains-connected system, it can be risky or impossible to get to the UART port. Things like voltage isolation, ground loop etc., should be taken care of and planned ahead or you may get devices destroyed.
- Not enough pin out for UART output.

On these occasions, a logging backend via BLE would be extremely valuable. The log backend was written for the Zephyr logging system and built on top of the Nordic Uart Service.
