# Graph, copy and allocation contracts run with persistent RTL caches and RTL Tensor arithmetic.
# Graph、拷贝和分配契约在持久 RTL 缓存及 RTL Tensor 算术下运行。
import unittest
import test_rtl_host as tensor


class CacheDevice:
    def device(self, **options):
        device = super().device(cache_rtl=True, **options)
        self.assertEqual(device.properties['cache_backend'], 'rtl')
        return device


class CacheHostContracts(CacheDevice, tensor.RtlHostContracts):
    pass


class CacheGraphContracts(CacheDevice, tensor.RtlGraphContracts):
    pass


class CachePersistentHardware(CacheDevice, tensor.RtlPersistentHardware):
    pass


class CacheHostExecution(CacheDevice, tensor.RtlHostExecution):
    pass


if __name__ == '__main__':
    unittest.main(verbosity=2)
