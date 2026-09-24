"""Windows GATT peripheral that advertises the Elite 300's service.

Service 0xFF00, notify 0xFF01, write 0xFF02. Subscribing starts a DeviceSession;
each write is fed to it and the notifications it returns are sent back in order.
An asyncio loop on a background thread owns the WinRT objects. The GUI hears
about it through `events`, a queue of ("status", text) and ("log", text).
"""
import asyncio
import queue
import threading
import uuid

from winrt.system import Array
from winrt.windows.devices.bluetooth import BluetoothError
from winrt.windows.devices.bluetooth.genericattributeprofile import (
    GattCharacteristicProperties, GattCommunicationStatus,
    GattLocalCharacteristicParameters, GattProtectionLevel,
    GattServiceProvider, GattServiceProviderAdvertisingParameters,
    GattWriteOption)
from winrt.windows.storage.streams import DataReader, DataWriter

from protocol import DeviceSession

_BASE = "0000{:04x}-0000-1000-8000-00805f9b34fb"
SVC_UUID = uuid.UUID(_BASE.format(0xFF00))
NOTIFY_UUID = uuid.UUID(_BASE.format(0xFF01))
WRITE_UUID = uuid.UUID(_BASE.format(0xFF02))

# PEER_PUBKEY is a 146-byte notify, which needs an ATT MTU of at least 149.
MIN_MTU = 149


def to_buffer(data):
    writer = DataWriter()
    writer.write_bytes(data)
    return writer.detach_buffer()


def from_buffer(buf):
    arr = Array("B", buf.length)
    DataReader.from_buffer(buf).read_bytes(arr)
    return bytes(arr)


class BleServer:
    def __init__(self, regs, events=None):
        self.regs = regs
        self.events = events if events is not None else queue.Queue()
        self._thread = None
        self._loop = None
        self._started = threading.Event()
        self._start_error = None
        self._stop = None
        self._provider = None
        self._notify = None
        self._session = None
        self._gatt = None

    def start(self):
        self._thread = threading.Thread(target=self._thread_main, daemon=True)
        self._thread.start()
        if not self._started.wait(15):
            raise TimeoutError("BLE server did not start")
        if self._start_error:
            raise self._start_error

    def stop(self):
        if self._loop and self._stop:
            self._loop.call_soon_threadsafe(self._stop.set)

    def disconnect(self):
        """Drop the connected client so the HMI has to reconnect."""
        if self._loop:
            self._spawn(self._disconnect())

    def _spawn(self, coro):
        """Run coro on the BLE loop from any thread; log it if it raises."""
        fut = asyncio.run_coroutine_threadsafe(coro, self._loop)
        fut.add_done_callback(
            lambda f: f.cancelled() or f.exception() is None
            or self._log(f"error: {f.exception()!r}"))

    def _event(self, kind, text):
        self.events.put((kind, text))

    def _log(self, text):
        self._event("log", text)

    def _thread_main(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._serve())
        finally:
            self._loop.close()

    async def _serve(self):
        self._stop = asyncio.Event()
        try:
            result = await GattServiceProvider.create_async(SVC_UUID)
            if result.error != BluetoothError.SUCCESS:
                raise RuntimeError(f"create service failed ({result.error})")
            self._provider = result.service_provider
            self._notify = await self._add_char(
                NOTIFY_UUID, GattCharacteristicProperties.NOTIFY)
            write = await self._add_char(
                WRITE_UUID,
                GattCharacteristicProperties.WRITE |
                GattCharacteristicProperties.WRITE_WITHOUT_RESPONSE)
            self._notify.add_subscribed_clients_changed(self._on_subscribed)
            write.add_write_requested(self._on_write)

            adv = GattServiceProviderAdvertisingParameters()
            adv.is_connectable = True
            adv.is_discoverable = True
            self._provider.start_advertising_with_parameters(adv)
        except Exception as exc:
            self._start_error = exc
            self._started.set()
            return

        self._event("status", "advertising")
        self._log("advertising service FF00")
        self._started.set()
        await self._stop.wait()
        await self._disconnect()
        self._provider.stop_advertising()
        self._event("status", "stopped")

    async def _add_char(self, uid, props):
        params = GattLocalCharacteristicParameters()
        params.characteristic_properties = props
        params.read_protection_level = GattProtectionLevel.PLAIN
        params.write_protection_level = GattProtectionLevel.PLAIN
        result = await self._provider.service.create_characteristic_async(uid, params)
        if result.error != BluetoothError.SUCCESS:
            raise RuntimeError(f"create characteristic {uid} failed ({result.error})")
        return result.characteristic

    def _on_subscribed(self, sender, _args):
        clients = list(sender.subscribed_clients)
        self._spawn(self._subscribed(clients))

    async def _subscribed(self, clients):
        if not clients:
            self._session = None
            self._gatt = None
            self._event("status", "advertising")
            self._log("client unsubscribed")
            return
        # A stale subscriber dropping out must not reset the live session.
        tracked = self._gatt.device_id if self._gatt is not None else None
        if tracked and any(c.session.device_id == tracked for c in clients):
            return
        client = clients[-1]
        self._gatt = client.session
        self._log_mtu(client.session.max_pdu_size)
        client.session.add_max_pdu_size_changed(self._on_mtu)
        self._session = DeviceSession(self.regs, log=self._log)
        self._event("status", "handshaking")
        for note in self._session.start():
            await self._notify_bytes(note)

    def _on_mtu(self, session, _args):
        self._loop.call_soon_threadsafe(self._log_mtu, session.max_pdu_size)

    def _log_mtu(self, mtu):
        extra = "" if mtu >= MIN_MTU else " (too small for the key exchange)"
        self._log(f"MTU {mtu}{extra}")

    def _on_write(self, _sender, args):
        deferral = args.get_deferral()
        self._spawn(self._written(args, deferral))

    async def _written(self, args, deferral):
        try:
            request = await args.get_request_async()
            if request is None:
                return
            # Always acknowledge: the HMI blocks on the ATT response, so an
            # unanswered write would stall it for the 30 s ATT timeout instead
            # of letting it time out the reply and reconnect.
            if request.option == GattWriteOption.WRITE_WITH_RESPONSE:
                request.respond()
            if self._session is None:
                return
            data = from_buffer(request.value)
            notes = self._session.handle(data)
            for note in notes:
                await self._notify_bytes(note)
            if self._session.state == DeviceSession.SECURE:
                self._event("status", "connected")
            elif self._session.state == DeviceSession.DEAD:
                self._event("status", "handshake failed")
        finally:
            deferral.complete()

    async def _notify_bytes(self, payload):
        # One result per subscribed client; a short bytes_sent means the
        # notification was cut to the link's MTU.
        results = await self._notify.notify_value_async(to_buffer(payload))
        for r in results:
            if r.status != GattCommunicationStatus.SUCCESS:
                self._log(f"notify failed ({r.status}), {len(payload)} bytes")
            elif r.bytes_sent < len(payload):
                self._log(f"notify truncated: {r.bytes_sent} of {len(payload)} bytes")

    async def _disconnect(self):
        gatt, self._gatt = self._gatt, None
        self._session = None
        if gatt is not None:
            gatt.close()
            self._log("client disconnected")
            self._event("status", "advertising")
