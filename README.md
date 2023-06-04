# Log Backend using Nordic Uart Service

Logging is an indispensible tool for firmware development. In particular, when debugging a BT stack, only single step breakpoints are usable as resuming stopped code execution causes kernel oops in Zephyr.

There are at least three use cases.

- If the UART is used for protocol communication with another chip, using it as log backend can be tricky as it requires the other device to ignore log outputs.
- When debugging BT devices getting power from a mains-connected system, it can be risky or impossible to get to the UART port. Things like voltage isolation, ground loop etc., should be taken care of and planned ahead or you may get devices destroyed.
- Not enough pin out for UART output.

On these occasions, a logging backend via BLE would be extremely valuable. The log backend was written for the Zephyr logging system and built on top of the Nordic Uart Service.

## Add the backend to your source code

1. Include _log_backend_nus.h_ in the main source file.
2. Add the following lines to the startup sequence in your main function.

```cpp
		if (IS_ENABLED(CONFIG_LOG_BACKEND_NUS)) {
			register_bt_nus_auth_cbs();
			nus_init();
		}
```

Note: when BLE Mesh and BLE peripheral coexist, the advertisement of the NUS service may interfere with the Mesh provision process. To mitigate this, wrap the code above in an if statement. This will prevent NUS from advertising until the BLE Mesh has been provisioned.

```cpp
	if (bt_mesh_is_provisioned())
		// Don't start advertising until mesh is provisioned
	{
		if (IS_ENABLED(CONFIG_LOG_BACKEND_NUS)) {
			register_bt_nus_auth_cbs();
			nus_init();
		}

	}
```

3. Create a new Kconfig file under the project root if not present and add the following line to the end

```
    rsource "log_backend_nus/Kconfig"
```

4. Include overlay-bt-nus.conf as Kconfig fragment to your build config.
5. Make sure CONFIG_LOG is enabled.
6. Finally you may want to tweak your prj.conf file. The following settings may be relevant

- CONFIG_HEAP_MEM_POOL_SIZE: the NUS service uses a FIFO with k_malloc to queue up messages to be transmitted. The pool size will limit the amount of unsent messages at any given moment in time. Note that this pool is shared with any other objects created by k_malloc.

- CONFIG_BT_L2CAP_TX_MTU: this will impact the maximum length of a single NUS message, and in turn the throughput.

7. The backend starts automatically and pairs with any NUS client. Once the connection is established, it start transmitting log message.
