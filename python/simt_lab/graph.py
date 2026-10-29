# Graph templates, executable snapshots, and launches are separate host-side objects.
# 图模板、可执行快照和启动是分开的主机对象。
from dataclasses import dataclass
from .host_types import Event, Operation
from .transport import DeviceError


class Graph:
    def __init__(self, device):
        self.device = device
        self.nodes = []

    # Explicit dependencies may be added later; the C++ instantiation validates the full DAG.
    # 可在之后增加显式依赖，由 C++ 实例化验证完整 DAG。
    def add(self, operation, dependencies=()):
        if operation.device is not self.device:
            raise DeviceError('foreign_device_operation')
        if operation.fields[0] == 'GRAPH':
            raise DeviceError('nested_graph_unsupported')
        operation.encode()
        self.nodes.append((operation,tuple(dependencies)))
        return len(self.nodes)-1

    def depends_on(self, node, dependency):
        if not 0 <= node < len(self.nodes) or not 0 <= dependency < len(self.nodes):
            raise DeviceError('invalid_graph_node')
        operation,dependencies = self.nodes[node]
        self.nodes[node] = operation,(*dependencies,dependency)

    # Instantiation freezes operands and topology instead of retaining the mutable template.
    # 实例化冻结操作数和拓扑，不直接保留可变图模板。
    def instantiate(self):
        fields = ['GRAPH',str(len(self.nodes))]
        for operation,dependencies in self.nodes:
            fields.extend(map(str,(len(dependencies),*dependencies)))
            fields.append(operation.encode())
        handle = self.device._request(' '.join(fields))
        return GraphExec(self.device,handle,[operation for operation,_ in self.nodes])


class GraphExec:
    def __init__(self, device, handle, operations):
        self.device, self.handle = device,handle
        self.operations = list(operations)

    # One public submission launches the retained graph; its nodes are expanded by the CP.
    # 一次公开提交启动保留的图，节点由 CP 展开。
    def launch(self, *, stream=0, wait_for=()):
        buffers = tuple(buffer for operation in self.operations for buffer in operation.buffers)
        operation = Operation(self.device,('GRAPH',self.handle),buffers)
        event = self.device.submit(operation,stream=stream,wait_for=wait_for)
        return GraphRun(event.device,event.id)

    # Backend validation prevents in-flight changes and node-type or topology changes.
    # 后端验证禁止运行中的修改，以及节点类型或拓扑变化。
    def update(self, node, operation):
        if operation.device is not self.device:
            raise DeviceError('foreign_device_operation')
        self.device._request(f'UPDATE {self.handle} {node} '+operation.encode())
        self.operations[node] = operation


@dataclass(frozen=True)
class GraphRun(Event):
    # Internal node handles become available after the CP has expanded the parent packet.
    # CP 展开父包后，内部节点句柄才可用。
    def node_event(self, node):
        children = self.query()['children']
        if not 0 <= node < len(children):
            raise DeviceError('graph_node_not_available')
        return Event(self.device,children[node])

    def node_result(self, node, max_ticks=1000000):
        self.wait(max_ticks)
        return self.node_event(node).result(max_ticks)


@dataclass(frozen=True)
class CapturedEvent:
    capture: object
    node: int


class Capture:
    # The first stream is the capture origin; other participating streams must rejoin it.
    # 第一个 stream 是捕获起点，其他参与 stream 必须重新汇入它。
    def __init__(self, device, streams):
        self.device = device
        self.streams = tuple(streams)
        if not self.streams or len(set(self.streams)) != len(self.streams):
            raise DeviceError('invalid_capture_streams')
        if any(not isinstance(s,int) or not 0 <= s <= 0xFFFFFFFF for s in self.streams):
            raise DeviceError('invalid_capture_streams')
        self.template = Graph(device)
        self.tails = {}
        self.graph = None

    # Capture calls produce graph nodes and never occupy the device submission queue.
    # 捕获调用产生图节点，从不占用设备提交队列。
    def record(self, operation, stream, wait_for):
        if stream not in self.streams:
            raise DeviceError('stream_not_in_capture')
        dependencies = [self.tails[stream]] if stream in self.tails else []
        for event in wait_for:
            if not isinstance(event,CapturedEvent) or event.capture is not self:
                raise DeviceError('event_not_in_capture')
            dependencies.append(event.node)
        node = self.template.add(operation,dependencies)
        self.tails[stream] = node
        return CapturedEvent(self,node)

    # Find ancestors without sharing the backend's cycle-check implementation.
    # 查找祖先节点，不复用后端的环路检查实现。
    def ancestors(self, node):
        seen, pending = set(),[node]
        while pending:
            current = pending.pop()
            if current in seen:
                continue
            seen.add(current)
            pending.extend(self.template.nodes[current][1])
        return seen

    # Cross-stream capture ends only after all participating branches rejoin the origin.
    # 跨 stream 捕获仅在所有参与分支重新汇入起点后结束。
    def finish(self):
        if not self.tails:
            self.graph = self.template
            return
        origin = self.tails.get(self.streams[0])
        if origin is None or any(tail not in self.ancestors(origin) for tail in self.tails.values()):
            raise DeviceError('capture_unjoined_stream')
        self.graph = self.template

    def __enter__(self):
        if self.device._capture is not None:
            raise DeviceError('nested_capture_unsupported')
        if self.device.closed:
            raise DeviceError('device_closed')
        self.device._capture = self
        return self

    # A failed capture discards its template and always restores ordinary submission mode.
    # 捕获失败时丢弃模板，并始终恢复普通提交模式。
    def __exit__(self, kind, value, traceback):
        self.device._capture = None
        if kind is None:
            self.finish()
