# Run the complete host, Graph and persistent-hardware contracts through the CP circuit.
# 通过 CP 电路运行完整的主机、Graph 和持久硬件契约。
import unittest
import test_timed_host as timed


class CircuitDevice:
    def device(self, **options):
        device = super().device(register_rtl=True, collector_rtl=True, **options)
        self.assertEqual(device.stats()['cp_backend'], 'rtl')
        return device


class CircuitHostContracts(CircuitDevice, timed.TimedHostContracts):
    pass


class CircuitGraphContracts(CircuitDevice, timed.TimedGraphContracts):
    pass


class CircuitPersistentHardware(CircuitDevice, timed.PersistentHardwareTest):
    pass


if __name__ == '__main__':
    unittest.main()
