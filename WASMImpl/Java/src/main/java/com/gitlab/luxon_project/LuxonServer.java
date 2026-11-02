package com.gitlab.luxon_project;

import com.dylibso.chicory.runtime.ExportFunction;
import com.dylibso.chicory.runtime.HostFunction;
import com.dylibso.chicory.runtime.ImportFunction;
import com.dylibso.chicory.runtime.ImportValues;
import com.dylibso.chicory.runtime.Instance;
import com.dylibso.chicory.runtime.Memory;
import com.dylibso.chicory.wasi.WasiOptions;
import com.dylibso.chicory.wasi.WasiPreview1;
import com.dylibso.chicory.wasm.WasmModule;
import com.dylibso.chicory.wasm.types.ValueType;
import org.jetbrains.annotations.NotNull;
import org.jetbrains.annotations.Nullable;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.channels.DatagramChannel;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.channels.UnsupportedAddressTypeException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class LuxonServer {
    private static final int O_NONBLOCK = 0x00000800;
    private static final int F_GETFL = 3;
    private static final int F_SETFL = 4;
    
    private static final int AF_UNSPEC = 0;
    private static final int AF_INET = 2;
    private static final int AF_INET6 = 10;
    
    private static final int SOCK_STREAM = 1;
    private static final int SOCK_DGRAM = 2;
    
    private static final int IPPROTO_TCP = 6;
    private static final int IPPROTO_UDP = 17;
    
    private static final int AI_PASSIVE = 0x0001;
    private static final int AI_CANONNAME = 0x0002;
    private static final int AI_NUMERICHOST = 0x0004;
    private static final int AI_V4MAPPED = 0x0008;
    private static final int AI_ALL = 0x0010;
    private static final int AI_ADDRCONFIG = 0x0020;   // accepted, ignored
    private static final int AI_NUMERICSERV = 0x0400;
    
    private static final int EAI_BADFLAGS = -1;
    private static final int EAI_NONAME = -2;
    private static final int EAI_AGAIN = -3;
    private static final int EAI_FAIL = -4;
    private static final int EAI_FAMILY = -6;
    private static final int EAI_SOCKTYPE = -7;
    private static final int EAI_SERVICE = -8;
    private static final int EAI_MEMORY = -10;
    
    private static final int SOCKADDR_IN_LEN = 16;
    private static final int SOCKADDR_IN6_LEN = 28;
    private static final int ADDRINFO_LEN = 32;
    
    // Emulated POSIX descriptor map starting at 100 to clear standard WASI descriptors (0-3)
    private static final Map<Integer, SocketHandle> sockets = new HashMap<>();
    private static int nextFileDescriptor = 100;
    
    private static class SocketHandle {
        final int fileDescriptor;
        final int addressFamily;
        boolean isServer = false;
        boolean isUdp;
        boolean nonBlocking = false;
        ServerSocketChannel serverChannel;
        SocketChannel socketChannel;
        DatagramChannel datagramChannel;
        
        SocketHandle(int fileDescriptor, int addressFamily, boolean isUdp) {
            this.fileDescriptor = fileDescriptor;
            this.addressFamily = addressFamily;
            this.isUdp = isUdp;
        }
    }
    
    public static void main(@NotNull String @NotNull [] args) {
        try {
            @NotNull WasiOptions wasiOptions = WasiOptions.builder()
                .withStdout(System.out)
                .withStderr(System.err)
                .withDirectory(".", Path.of("."))
                .build();
            
            //noinspection resource
            @NotNull List<@NotNull ImportFunction> functions = new ArrayList<>(Arrays.asList(WasiPreview1.builder().withOptions(wasiOptions).build().toHostFunctions()));
            
            @Nullable ImportFunction origPathOpen = null;
            @Nullable ImportFunction origFdRead = null;
            @Nullable ImportFunction origFdClose = null;
            
            for (@NotNull ImportFunction function : functions) {
                if ("wasi_snapshot_preview1".equals(function.module())) {
                    if ("path_open".equals(function.name())) origPathOpen = function;
                    else if ("fd_read".equals(function.name())) origFdRead = function;
                    else if ("fd_close".equals(function.name())) origFdClose = function;
                }
            }
            
            final @Nullable ImportFunction finalPathOpen = origPathOpen;
            final @Nullable ImportFunction finalFdRead = origFdRead;
            final @Nullable ImportFunction finalFdClose = origFdClose;
            
            // Reserve a high virtual FD number that won't collide with WASI or sockets
            final int RANDOM_FD = 9999;
            final @NotNull SecureRandom secureRandom = new SecureRandom();
            
            if (finalPathOpen != null) {
                functions.remove(finalPathOpen);
                functions.add(new HostFunction(finalPathOpen.module(), finalPathOpen.name(), finalPathOpen.paramTypes(), finalPathOpen.returnTypes(), (instance, wasmArguments) -> {
                    int pathPtr = (int) wasmArguments[2];
                    int pathLen = (int) wasmArguments[3];
                    int resultFdPtr = (int) wasmArguments[8];
                    
                    @NotNull Memory mem = instance.memory();
                    @NotNull String path = new String(mem.readBytes(pathPtr, pathLen), StandardCharsets.UTF_8);
                    
                    if (path.endsWith("/dev/random") || path.endsWith("/dev/urandom") || path.equals("dev/random") || path.equals("dev/urandom")) {
                        writeIntLE(mem, resultFdPtr, RANDOM_FD);
                        return new long[]{0}; // WASI_ESUCCESS
                    }
                    return finalPathOpen.handle().apply(instance, wasmArguments);
                }));
            }
            
            if (finalFdRead != null) {
                functions.remove(finalFdRead);
                functions.add(new HostFunction(finalFdRead.module(), finalFdRead.name(), finalFdRead.paramTypes(), finalFdRead.returnTypes(), (instance, wasmArguments) -> {
                    int fd = (int) wasmArguments[0];
                    if (fd == RANDOM_FD) {
                        int iovsPtr = (int) wasmArguments[1];
                        int iovsLen = (int) wasmArguments[2];
                        int resultSizePtr = (int) wasmArguments[3];
                        @NotNull Memory memory = instance.memory();
                        
                        int totalRead = 0;
                        for (int i = 0; i < iovsLen; i++) {
                            int bufferPointer = readIntLE(memory, iovsPtr + (i * 8));
                            int bufferLength = readIntLE(memory, iovsPtr + (i * 8) + 4);
                            
                            byte @NotNull [] randomBytes = new byte[bufferLength];
                            secureRandom.nextBytes(randomBytes);
                            memory.write(bufferPointer, randomBytes);
                            totalRead += bufferLength;
                        }
                        writeIntLE(memory, resultSizePtr, totalRead);
                        return new long[]{0}; // WASI_ESUCCESS
                    }
                    return finalFdRead.handle().apply(instance, wasmArguments);
                }));
            }
            
            if (finalFdClose != null) {
                functions.remove(finalFdClose);
                functions.add(new HostFunction(finalFdClose.module(), finalFdClose.name(), finalFdClose.paramTypes(), finalFdClose.returnTypes(), (instance, wasmArgs) -> {
                    int fileDescriptor = (int) wasmArgs[0];
                    if (fileDescriptor == RANDOM_FD) {
                        return new long[]{0}; // WASI_ESUCCESS
                    }
                    return finalFdClose.handle().apply(instance, wasmArgs);
                }));
            }
            
            functions.addAll(registerSocketEnvironmentImports());
            
            @NotNull ImportValues imports = ImportValues.builder()
                .withFunctions(functions)
                .build();
            
            @NotNull WasmModule module = LuxonServerModule.load();
            @NotNull Instance instance = Instance.builder(module)
                .withMachineFactory(LuxonServerModule::create)
                .withImportValues(imports)
                .build();
            
            @Nullable ExportFunction startFunction = instance.export("_start");
            if (startFunction != null) {
                startFunction.apply();
            }
            
        } catch (@NotNull Exception exception) {
            System.err.println("Fatal execution error inside WASM host wrapper: " + exception.getMessage());
            exception.printStackTrace();
            System.exit(1);
        }
    }
    
    private static @NotNull List<@NotNull ImportFunction> registerSocketEnvironmentImports() {
        final @NotNull List<@NotNull ImportFunction> environment = new ArrayList<>();
        final @NotNull String namespace = "env";
        
        // u32 w2c_env_socket_socket(domain, type, protocol) -> 3 params
        environment.add(new HostFunction(namespace, "socket_socket", params(3), returns(1), (instance, args) -> {
            int domain = (int) args[0];
            int type = (int) args[1];
            boolean isUdp = type == SOCK_DGRAM;
            int fileDescriptor = nextFileDescriptor++;
            sockets.put(fileDescriptor, new SocketHandle(fileDescriptor, domain, isUdp));
            return new long[]{fileDescriptor};
        }));
        
        // u32 w2c_env_socket_bind(sockfd, addr_ptr, addrlen) -> 3 params
        environment.add(new HostFunction(namespace, "socket_bind", params(3), returns(1), (instance, args) -> {
            int socketFileDescriptor = (int) args[0];
            int addressPointer = (int) args[1];
            int addressLength = (int) args[2];
            @Nullable SocketHandle handle = sockets.get(socketFileDescriptor);
            if (handle == null) return new long[]{-1L};
            
            try {
                @NotNull Memory memory = instance.memory();
                int addressFamily = memory.readBytes(addressPointer, 1)[0] & 0xFF;
                int port = ((memory.readBytes(addressPointer + 2, 2)[0] & 0xFF) << 8) | (memory.readBytes(addressPointer + 2, 2)[1] & 0xFF);
                
                @NotNull InetAddress address = (addressFamily == AF_INET6 || addressLength == 28) ?
                    InetAddress.getByAddress(memory.readBytes(addressPointer + 8, 16)) :
                    InetAddress.getByAddress(memory.readBytes(addressPointer + 4, 4));
                
                try {
                    if (handle.isUdp) {
                        handle.datagramChannel = DatagramChannel.open();
                        handle.datagramChannel.configureBlocking(!handle.nonBlocking);
                        handle.datagramChannel.bind(new InetSocketAddress(address, port));
                    } else {
                        handle.isServer = true;
                        handle.serverChannel = ServerSocketChannel.open();
                        handle.serverChannel.configureBlocking(!handle.nonBlocking);
                        handle.serverChannel.bind(new InetSocketAddress(address, port));
                    }
                } catch (UnsupportedAddressTypeException exception) {
                    @Nullable InetAddress fallback = getFallbackAddress(address);
                    if (fallback != null) {
                        System.err.println("WARNING: Unsupported address type for bind. Falling back from " + address + " to " + fallback);
                        if (handle.isUdp) {
                            handle.datagramChannel.bind(new InetSocketAddress(fallback, port));
                        } else {
                            handle.serverChannel.bind(new InetSocketAddress(fallback, port));
                        }
                    } else {
                        throw exception;
                    }
                }
                return new long[]{0};
            } catch (@NotNull Exception exception) {
                return new long[]{-1L};
            }
        }));
        
        // u32 w2c_env_socket_listen(sockfd, backlog) -> 2 params
        environment.add(new HostFunction(namespace, "socket_listen", params(2), returns(1), (instance, args) -> {
            @Nullable SocketHandle handle = sockets.get((int) args[0]);
            if (handle != null && handle.isUdp) return new long[]{0};
            return new long[]{(handle != null && handle.isServer) ? 0 : -1L};
        }));
        
        // u32 w2c_env_socket_accept(sockfd, addr_ptr, addrlen_ptr) -> 3 params
        environment.add(new HostFunction(namespace, "socket_accept", params(3), returns(1), (instance, args) -> {
            int socketFileDescriptor = (int) args[0];
            int addressPointer = (int) args[1];
            int addressLengthPointer = (int) args[2];
            
            @Nullable SocketHandle handle = sockets.get(socketFileDescriptor);
            if (handle == null || handle.isUdp || !handle.isServer || handle.serverChannel == null) {
                return new long[]{-1L};
            }
            
            // POSIX ABI: if addr != NULL, addrlen must be a valid value-result pointer.
            if (addressPointer != 0 && addressLengthPointer == 0) {
                return new long[]{-1L};
            }
            
            try {
                @Nullable SocketChannel client = handle.serverChannel.accept();
                if (client == null) {
                    return new long[]{-1L};
                }
                
                client.configureBlocking(true);
                
                int clientFileDescriptor = nextFileDescriptor++;
                @NotNull SocketHandle clientHandle = new SocketHandle(clientFileDescriptor, handle.addressFamily, false);
                clientHandle.socketChannel = client;
                sockets.put(clientFileDescriptor, clientHandle);
                
                if (addressPointer != 0) {
                    @Nullable SocketAddress remote = client.getRemoteAddress();
                    if (!(remote instanceof InetSocketAddress) ||
                        writeSockaddrResult(instance.memory(), addressPointer, addressLengthPointer, (InetSocketAddress) remote, handle.addressFamily)) {
                        closeHandle(clientFileDescriptor);
                        return new long[]{-1L};
                    }
                }
                
                return new long[]{clientFileDescriptor};
            } catch (@NotNull IOException e) {
                return new long[]{-1L};
            }
        }));
        
        // u32 w2c_env_socket_connect(sockfd, addr_ptr, addrlen) -> 3 params
        environment.add(new HostFunction(namespace, "socket_connect", params(3), returns(1), (instance, args) -> {
            int socketFileDescriptor = (int) args[0];
            int addressPointer = (int) args[1];
            int addressLength = (int) args[2];
            @Nullable SocketHandle handle = sockets.get(socketFileDescriptor);
            if (handle == null) return new long[]{-1L};
            
            try {
                @NotNull Memory memory = instance.memory();
                int addressFamily = memory.readBytes(addressPointer, 1)[0] & 0xFF;
                int port = ((memory.readBytes(addressPointer + 2, 2)[0] & 0xFF) << 8) | (memory.readBytes(addressPointer + 2, 2)[1] & 0xFF);
                @NotNull InetAddress address = (addressFamily == AF_INET6 || addressLength == 28) ?
                    InetAddress.getByAddress(memory.readBytes(addressPointer + 8, 16)) :
                    InetAddress.getByAddress(memory.readBytes(addressPointer + 4, 4));
                
                try {
                    if (handle.isUdp) {
                        handle.datagramChannel = DatagramChannel.open();
                        handle.datagramChannel.configureBlocking(!handle.nonBlocking);
                        handle.datagramChannel.connect(new InetSocketAddress(address, port));
                        return new long[]{0};
                    } else {
                        handle.socketChannel = SocketChannel.open();
                        handle.socketChannel.configureBlocking(!handle.nonBlocking);
                        boolean success = handle.socketChannel.connect(new InetSocketAddress(address, port));
                        return new long[]{success ? 0 : -1L};
                    }
                } catch (@NotNull UnsupportedAddressTypeException exception) {
                    @Nullable InetAddress fallback = getFallbackAddress(address);
                    if (fallback != null) {
                        System.err.println("WARNING: Unsupported address type for connect. Falling back from " + address + " to " + fallback);
                        if (handle.isUdp) {
                            handle.datagramChannel.connect(new InetSocketAddress(fallback, port));
                            return new long[]{0};
                        } else {
                            boolean success = handle.socketChannel.connect(new InetSocketAddress(fallback, port));
                            return new long[]{success ? 0 : -1L};
                        }
                    } else {
                        throw exception; // Rethrow if we can't formulate a valid fallback
                    }
                }
            } catch (@NotNull Exception exception) {
                return new long[]{-1L};
            }
        }));
        
        environment.add(new HostFunction(namespace, "socket_send", params(4), returns(1), (instance, wasmArguments) ->
            doSend(instance, (int) wasmArguments[0], (int) wasmArguments[1], (int) wasmArguments[2], (int) wasmArguments[3], 0, 0)
        ));
        
        environment.add(new HostFunction(namespace, "socket_recv", params(4), returns(1), (instance, wasmArguments) ->
            doRecv(instance, (int) wasmArguments[0], (int) wasmArguments[1], (int) wasmArguments[2], (int) wasmArguments[3], 0, 0)
        ));
        
        environment.add(new HostFunction(namespace, "socket_sendto", params(6), returns(1), (instance, wasmArguments) ->
            doSend(instance, (int) wasmArguments[0], (int) wasmArguments[1], (int) wasmArguments[2], (int) wasmArguments[3], (int) wasmArguments[4], (int) wasmArguments[5])
        ));
        
        environment.add(new HostFunction(namespace, "socket_recvfrom", params(6), returns(1), (instance, wasmArguments) ->
            doRecv(instance, (int) wasmArguments[0], (int) wasmArguments[1], (int) wasmArguments[2], (int) wasmArguments[3], (int) wasmArguments[4], (int) wasmArguments[5])
        ));
        
        environment.add(new HostFunction(namespace, "socket_setsockopt", params(5), returns(1), (instance, wasmArguments) -> new long[]{0}));
        environment.add(new HostFunction(namespace, "socket_shutdown", params(2), returns(1), (instance, wasmArguments) -> closeHandle((int) wasmArguments[0])));
        environment.add(new HostFunction(namespace, "socket_close", params(1), returns(1), (instance, wasmArguments) -> closeHandle((int) wasmArguments[0])));
        
        // u32 w2c_env_socket_fcntl(fd, cmd, arg) -> 3 params
        environment.add(new HostFunction(namespace, "socket_fcntl", params(3), returns(1), (instance, wasmArguments) -> {
            @Nullable SocketHandle handle = sockets.get((int) wasmArguments[0]);
            if (handle == null) return new long[]{-1L};
            int cmd = (int) wasmArguments[1];
            int arg = (int) wasmArguments[2];
            
            if (cmd == F_GETFL) {
                return new long[]{handle.nonBlocking ? O_NONBLOCK : 0};
            } else if (cmd == F_SETFL) {
                handle.nonBlocking = (arg & O_NONBLOCK) != 0;
                try {
                    if (handle.serverChannel != null) handle.serverChannel.configureBlocking(!handle.nonBlocking);
                    if (handle.datagramChannel != null) handle.datagramChannel.configureBlocking(!handle.nonBlocking);
                    if (handle.socketChannel != null) handle.socketChannel.configureBlocking(!handle.nonBlocking);
                    return new long[]{0};
                } catch (@NotNull IOException exception) {
                    return new long[]{-1L};
                }
            }
            return new long[]{-1L};
        }));
        
        environment.add(new HostFunction(namespace, "socket_ioctl", params(3), returns(1), (instance, wasmArguments) -> new long[]{0}));
        
        environment.add(new HostFunction(namespace, "socket_inet_pton", params(3), returns(1), (instance, wasmArguments) -> {
            int addressFamily = (int) wasmArguments[0];
            int sourcePointer = (int) wasmArguments[1];
            int destinationPointer = (int) wasmArguments[2];
            
            try {
                @NotNull Memory memory = instance.memory();
                @NotNull String source = readNullTerminatedString(memory, sourcePointer);
                
                byte[] parsed;
                if (addressFamily == AF_INET) {
                    parsed = parseIpv4Literal(source);
                } else if (addressFamily == AF_INET6) {
                    parsed = parseIpv6Literal(source);
                } else {
                    // POSIX: unsupported family => -1 / EAFNOSUPPORT.
                    return new long[]{-1L};
                }
                
                if (parsed == null) {
                    // Invalid presentation format.
                    return new long[]{0};
                }
                
                memory.write(destinationPointer, parsed);
                return new long[]{1};
            } catch (@NotNull Exception e) {
                return new long[]{0};
            }
        }));
        
        environment.add(new HostFunction(namespace, "socket_inet_ntop", params(4), returns(1), (instance, args) -> {
            try {
                @NotNull Memory memory = instance.memory();
                int addressFamily = (int) args[0];
                byte @NotNull [] ipBytes = memory.readBytes((int) args[1], addressFamily == AF_INET6 ? 16 : 4);
                byte @NotNull [] strBytes = InetAddress.getByAddress(ipBytes).getHostAddress().getBytes(StandardCharsets.UTF_8);
                if (strBytes.length + 1 <= (int) args[3]) {
                    memory.write((int) args[2], strBytes);
                    memory.write((int) args[2] + strBytes.length, new byte[]{0});
                    return new long[]{args[2]};
                }
            } catch (@NotNull Exception ignored) {
            }
            return new long[]{0};
        }));
        
        // Selector loop evaluating Read/Write states asynchronously
        environment.add(new HostFunction(namespace, "socket_select", params(5), returns(1), (instance, args) -> {
            int nfds = (int) args[0];
            int readfdsPtr = (int) args[1];
            int writefdsPtr = (int) args[2];
            int exceptfdsPtr = (int) args[3];
            int timeoutPointer = (int) args[4];
            @NotNull Memory memory = instance.memory();
            
            long timeoutMilliseconds = 0;
            boolean hasTimeout = false;
            if (timeoutPointer != 0) {
                hasTimeout = true;
                long seconds = readLongLE(memory, timeoutPointer);
                long milliseconds = readLongLE(memory, timeoutPointer + 8);
                timeoutMilliseconds = (seconds * 1000) + (milliseconds / 1000);
            }
            
            try (Selector selector = Selector.open()) {
                @NotNull Map<@NotNull SelectionKey, @NotNull Integer> keyFileDescriptorMap = new HashMap<>();
                
                int bytesToClear = (nfds + 7) / 8;
                
                byte[] rfds = readfdsPtr != 0 && bytesToClear > 0 ? memory.readBytes(readfdsPtr, bytesToClear) : null;
                byte[] wfds = writefdsPtr != 0 && bytesToClear > 0 ? memory.readBytes(writefdsPtr, bytesToClear) : null;
                
                if (readfdsPtr != 0 && bytesToClear > 0) memory.write(readfdsPtr, new byte[bytesToClear]);
                if (writefdsPtr != 0 && bytesToClear > 0) memory.write(writefdsPtr, new byte[bytesToClear]);
                if (exceptfdsPtr != 0 && bytesToClear > 0) memory.write(exceptfdsPtr, new byte[bytesToClear]);
                
                for (int fd = 0; fd < nfds; fd++) {
                    boolean wantsRead = rfds != null && (rfds[fd / 8] & (1 << (fd % 8))) != 0;
                    boolean wantsWrite = wfds != null && (wfds[fd / 8] & (1 << (fd % 8))) != 0;
                    
                    if (wantsRead || wantsWrite) {
                        @Nullable SocketHandle handle = sockets.get(fd);
                        if (handle != null) {
                            int operations = 0;
                            if (wantsRead) {
                                if (handle.isServer && handle.serverChannel != null)
                                    operations |= SelectionKey.OP_ACCEPT;
                                else operations |= SelectionKey.OP_READ;
                            }
                            if (wantsWrite && !handle.isServer) {
                                operations |= SelectionKey.OP_WRITE;
                            }
                            
                            if (operations != 0) {
                                @Nullable SelectionKey key = null;
                                if (handle.isServer && handle.serverChannel != null) {
                                    if (handle.serverChannel.isBlocking())
                                        handle.serverChannel.configureBlocking(false);
                                    key = handle.serverChannel.register(selector, operations);
                                } else if (handle.isUdp && handle.datagramChannel != null) {
                                    if (handle.datagramChannel.isBlocking())
                                        handle.datagramChannel.configureBlocking(false);
                                    key = handle.datagramChannel.register(selector, operations);
                                } else if (handle.socketChannel != null) {
                                    if (handle.socketChannel.isBlocking())
                                        handle.socketChannel.configureBlocking(false);
                                    key = handle.socketChannel.register(selector, operations);
                                }
                                if (key != null) keyFileDescriptorMap.put(key, fd);
                            }
                        }
                    }
                }
                
                int readyCount = 0;
                if (!keyFileDescriptorMap.isEmpty()) {
                    if (hasTimeout) {
                        if (timeoutMilliseconds == 0) selector.selectNow();
                        else selector.select(timeoutMilliseconds);
                    } else {
                        selector.select();
                    }
                    
                    for (@NotNull SelectionKey key : selector.selectedKeys()) {
                        int fd = keyFileDescriptorMap.get(key);
                        if (readfdsPtr != 0 && (key.isReadable() || key.isAcceptable())) {
                            byte @NotNull [] bytes = memory.readBytes(readfdsPtr + (fd / 8), 1);
                            bytes[0] |= (byte) (1 << (fd % 8));
                            memory.write(readfdsPtr + (fd / 8), bytes);
                            readyCount++;
                        }
                        if (writefdsPtr != 0 && key.isWritable()) {
                            byte @NotNull [] bytes = memory.readBytes(writefdsPtr + (fd / 8), 1);
                            bytes[0] |= (byte) (1 << (fd % 8));
                            memory.write(writefdsPtr + (fd / 8), bytes);
                            readyCount++;
                        }
                    }
                } else if (hasTimeout && timeoutMilliseconds > 0) {
                    Thread.sleep(timeoutMilliseconds);
                }
                
                for (@NotNull SelectionKey key : keyFileDescriptorMap.keySet()) key.cancel();
                selector.selectNow();
                
                for (int fileDescriptor : keyFileDescriptorMap.values()) {
                    @Nullable SocketHandle handle = sockets.get(fileDescriptor);
                    if (handle != null) {
                        if (handle.isServer && handle.serverChannel != null)
                            handle.serverChannel.configureBlocking(!handle.nonBlocking);
                        else if (handle.isUdp && handle.datagramChannel != null)
                            handle.datagramChannel.configureBlocking(!handle.nonBlocking);
                        else if (handle.socketChannel != null)
                            handle.socketChannel.configureBlocking(!handle.nonBlocking);
                    }
                }
                return new long[]{readyCount};
            } catch (@NotNull Exception e) {
                return new long[]{-1L};
            }
        }));
        
        environment.add(new HostFunction(namespace, "socket_getaddrinfo", params(4), returns(1), (instance, args) -> {
            @NotNull Memory memory = instance.memory();
            
            int nodePointer = (int) args[0];
            int servicePointer = (int) args[1];
            int hintsPointer = (int) args[2];
            int resourcePointer = (int) args[3];
            
            if (resourcePointer == 0) {
                return new long[]{EAI_FAIL};
            }
            writeIntLE(memory, resourcePointer, 0);
            
            @Nullable String node = nodePointer != 0 ? readNullTerminatedString(memory, nodePointer) : null;
            @Nullable String service = servicePointer != 0 ? readNullTerminatedString(memory, servicePointer) : null;
            
            if (node == null && service == null) {
                return new long[]{EAI_NONAME};
            }
            
            int flags = 0;
            int addressFamily = AF_UNSPEC;
            int socktype = 0;
            int protocol = 0;
            
            if (hintsPointer != 0) {
                flags = readIntLE(memory, hintsPointer);
                addressFamily = readIntLE(memory, hintsPointer + 4);
                socktype = readIntLE(memory, hintsPointer + 8);
                protocol = readIntLE(memory, hintsPointer + 12);
            }
            
            int supportedFlags =
                AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST |
                    AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG | AI_NUMERICSERV;
            
            if ((flags & ~supportedFlags) != 0) {
                return new long[]{EAI_BADFLAGS};
            }
            
            if (addressFamily != AF_UNSPEC && addressFamily != AF_INET && addressFamily != AF_INET6) {
                return new long[]{EAI_FAMILY};
            }
            
            if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM) {
                return new long[]{EAI_SOCKTYPE};
            }
            
            if (protocol != 0 && protocol != IPPROTO_TCP && protocol != IPPROTO_UDP) {
                return new long[]{EAI_SERVICE};
            }
            
            if ((socktype == SOCK_STREAM && protocol == IPPROTO_UDP) ||
                (socktype == SOCK_DGRAM && protocol == IPPROTO_TCP)) {
                return new long[]{EAI_SERVICE};
            }
            
            int port = 0;
            if (service != null) {
                @Nullable Integer parsedPort = parseNumericService(service);
                if (parsedPort == null) {
                    return new long[]{EAI_SERVICE};
                }
                port = parsedPort;
            }
            
            @NotNull List<@NotNull InetAddress> addresses;
            boolean numericNode;
            
            try {
                if (node == null) {
                    addresses = defaultAddressesForNullNode(addressFamily, (flags & AI_PASSIVE) != 0);
                } else {
                    byte[] v4 = parseIpv4Literal(node);
                    byte[] v6 = (v4 == null) ? parseIpv6Literal(node) : null;
                    numericNode = (v4 != null || v6 != null);
                    
                    if (numericNode) {
                        addresses = resolveNumericNode(addressFamily, flags, v4, v6);
                    } else {
                        if ((flags & AI_NUMERICHOST) != 0) {
                            return new long[]{EAI_NONAME};
                        }
                        addresses = resolveDnsNode(node, addressFamily, flags);
                    }
                }
            } catch (@NotNull UnknownHostException e) {
                return new long[]{EAI_NONAME};
            } catch (@NotNull Exception e) {
                return new long[]{EAI_FAIL};
            }
            
            if (addresses.isEmpty()) {
                return new long[]{EAI_NONAME};
            }
            
            @NotNull List<@NotNull SockProtoPair> pairs = buildSockProtoPairs(socktype, protocol);
            if (pairs.isEmpty()) {
                return new long[]{EAI_SERVICE};
            }
            
            @Nullable String canonicalName = ((flags & AI_CANONNAME) != 0 && node != null) ? node : null;
            
            @NotNull List<@NotNull AddrInfoResult> results = new ArrayList<>();
            boolean first = true;
            
            for (@NotNull InetAddress addr : addresses) {
                byte[] raw = addr.getAddress();
                int outFamily = raw.length == 16 ? AF_INET6 : AF_INET;
                int scopeId = (addr instanceof Inet6Address) ? ((Inet6Address) addr).getScopeId() : 0;
                byte[] sockaddr = encodeSockaddr(outFamily, raw, port, scopeId);
                
                for (@NotNull SockProtoPair pair : pairs) {
                    results.add(new AddrInfoResult(
                        outFamily,
                        pair.socketType,
                        pair.protocol,
                        sockaddr,
                        first ? canonicalName : null
                    ));
                    first = false;
                }
            }
            
            @Nullable ExportFunction malloc = findExport(instance, "malloc", "_malloc");
            @Nullable ExportFunction free = findExport(instance, "free", "_free");
            if (malloc == null) {
                return new long[]{EAI_MEMORY};
            }
            
            @NotNull List<@NotNull Integer> allocated = new ArrayList<>();
            
            try {
                int headPointer = 0;
                int previousPointer = 0;
                
                for (@NotNull AddrInfoResult r : results) {
                    int saPtr = guestMalloc(malloc, r.socketAddress.length);
                    if (saPtr == 0) throw new OutOfMemoryError();
                    allocated.add(saPtr);
                    memory.write(saPtr, r.socketAddress);
                    
                    int canonPtr = 0;
                    if (r.canonicalName != null) {
                        byte[] nameBytes = r.canonicalName.getBytes(StandardCharsets.UTF_8);
                        canonPtr = guestMalloc(malloc, nameBytes.length + 1);
                        if (canonPtr == 0) throw new OutOfMemoryError();
                        allocated.add(canonPtr);
                        memory.write(canonPtr, nameBytes);
                        memory.write(canonPtr + nameBytes.length, new byte[]{0});
                    }
                    
                    int addressInfoPointer = guestMalloc(malloc, ADDRINFO_LEN);
                    if (addressInfoPointer == 0) throw new OutOfMemoryError();
                    allocated.add(addressInfoPointer);
                    
                    writeIntLE(memory, addressInfoPointer, 0);
                    writeIntLE(memory, addressInfoPointer + 4, r.addressFamily);
                    writeIntLE(memory, addressInfoPointer + 8, r.socketType);
                    writeIntLE(memory, addressInfoPointer + 12, r.protocol);
                    writeIntLE(memory, addressInfoPointer + 16, r.socketAddress.length);
                    writeIntLE(memory, addressInfoPointer + 20, saPtr);
                    writeIntLE(memory, addressInfoPointer + 24, canonPtr);
                    writeIntLE(memory, addressInfoPointer + 28, 0);
                    
                    if (headPointer == 0) {
                        headPointer = addressInfoPointer;
                    } else {
                        writeIntLE(memory, previousPointer + 28, addressInfoPointer);
                    }
                    previousPointer = addressInfoPointer;
                }
                
                writeIntLE(memory, resourcePointer, headPointer);
                return new long[]{0};
            } catch (@NotNull Throwable throwable) {
                if (free != null) {
                    for (int i = allocated.size() - 1; i >= 0; i--) {
                        int ptr = allocated.get(i);
                        if (ptr != 0) free.apply(ptr);
                    }
                }
                writeIntLE(memory, resourcePointer, 0);
                return new long[]{EAI_MEMORY};
            }
        }));
        
        environment.add(new HostFunction(namespace, "socket_freeaddrinfo", params(1), returns(0), (instance, args) -> {
            @Nullable ExportFunction free = instance.export("free");
            if (free == null) free = instance.export("_free");
            if (free == null) return new long[0];
            
            @NotNull Memory mem = instance.memory();
            int currentArgument = (int) args[0];
            while (currentArgument != 0) {
                int address = readIntLE(mem, currentArgument + 20);
                int canon = readIntLE(mem, currentArgument + 24);
                int next = readIntLE(mem, currentArgument + 28);
                
                if (address != 0) free.apply(address);
                if (canon != 0) free.apply(canon);
                free.apply(currentArgument);
                currentArgument = next;
            }
            return new long[0];
        }));
        
        // u32 w2c_env_socket_getsockname(sockfd, addr_ptr, addrlen_ptr) -> 3 params
        environment.add(new HostFunction(namespace, "socket_getsockname", params(3), returns(1), (instance, args) -> {
            int socketFileDescriptor = (int) args[0];
            int addressPointer = (int) args[1];
            int addressLengthPointer = (int) args[2];
            
            @Nullable SocketHandle handle = sockets.get(socketFileDescriptor);
            if (handle == null) return new long[]{-1L};
            
            // POSIX ABI: if addr != NULL, addrlen must be a valid value-result pointer.
            if (addressPointer != 0 && addressLengthPointer == 0) {
                return new long[]{-1L};
            }
            
            try {
                @Nullable SocketAddress socketAddress = null;
                if (handle.isServer && handle.serverChannel != null) {
                    socketAddress = handle.serverChannel.getLocalAddress();
                } else if (handle.isUdp && handle.datagramChannel != null) {
                    socketAddress = handle.datagramChannel.getLocalAddress();
                } else if (handle.socketChannel != null) {
                    socketAddress = handle.socketChannel.getLocalAddress();
                }
                
                if (socketAddress == null) {
                    return new long[]{-1L};
                }
                
                if (addressPointer != 0) {
                    if (!(socketAddress instanceof InetSocketAddress) ||
                        writeSockaddrResult(instance.memory(), addressPointer, addressLengthPointer, (InetSocketAddress) socketAddress, handle.addressFamily)) {
                        return new long[]{-1L};
                    }
                }
                
                return new long[]{0};
            } catch (@NotNull IOException exception) {
                return new long[]{-1L};
            }
        }));
        
        return environment;
    }
    
    private static @Nullable InetAddress getFallbackAddress(@NotNull InetAddress address) {
        try {
            byte[] rawAddress = address.getAddress();
            if (rawAddress.length == 16) {
                // Check for an IPv4-mapped IPv6 address (::ffff:x.x.x.x)
                if (isIpv4MappedIpv6(rawAddress)) {
                    return InetAddress.getByAddress(extractMappedIpv4(rawAddress));
                }
                
                // Check for IPv6 wildcard address (::) and translate to IPv4 wildcard address (0.0.0.0)
                boolean isWildcardAddress = true;
                for (byte item : rawAddress) {
                    if (item != 0) {
                        isWildcardAddress = false;
                        break;
                    }
                }
                if (isWildcardAddress) {
                    return InetAddress.getByAddress(new byte[]{0, 0, 0, 0});
                }
                
                // Check for IPv6 loopback address (::1) and translate to to IPv4 loopback address (127.0.0.1)
                boolean isLoopback = true;
                for (int i = 0; i < 15; i++) {
                    if (rawAddress[i] != 0) {
                        isLoopback = false;
                        break;
                    }
                }
                if (isLoopback && rawAddress[15] == 1) {
                    return InetAddress.getByAddress(new byte[]{127, 0, 0, 1});
                }
                
            } else if (rawAddress.length == 4) {
                // Try promoting IPv4 up to IPv6 as a last resort
                return InetAddress.getByAddress(ipv4ToMappedIpv6(rawAddress));
            }
        } catch (@NotNull UnknownHostException ignored) {
        }
        return null;
    }
    
    private static long[] doSend(
        Instance instance,
        int socketFileDescriptor,
        int destinationBufferPointer,
        int desinationBufferLength,
        @SuppressWarnings("unused") int flags,
        int destinationAddressPointer,
        int destinationAddressLength
    ) {
        @Nullable SocketHandle handle = sockets.get(socketFileDescriptor);
        if (handle == null) return new long[]{-1L};
        try {
            @NotNull Memory memory = instance.memory();
            byte[] data = memory.readBytes(destinationBufferPointer, desinationBufferLength);
            @NotNull ByteBuffer buf = ByteBuffer.wrap(data);
            
            if (handle.isUdp && handle.datagramChannel != null) {
                if (destinationAddressPointer != 0) {
                    int addressFamily = memory.readBytes(destinationAddressPointer, 1)[0] & 0xFF;
                    int port = ((memory.readBytes(destinationAddressPointer + 2, 2)[0] & 0xFF) << 8) | (memory.readBytes(destinationAddressPointer + 2, 2)[1] & 0xFF);
                    @NotNull InetAddress addr = (addressFamily == AF_INET6 || destinationAddressLength == 28) ?
                        InetAddress.getByAddress(memory.readBytes(destinationAddressPointer + 8, 16)) :
                        InetAddress.getByAddress(memory.readBytes(destinationAddressPointer + 4, 4));
                    int sent = handle.datagramChannel.send(buf, new InetSocketAddress(addr, port));
                    return new long[]{sent};
                } else {
                    return new long[]{handle.datagramChannel.write(buf)};
                }
            } else if (handle.socketChannel != null) {
                return new long[]{handle.socketChannel.write(buf)};
            }
            return new long[]{-1L};
        } catch (@NotNull IOException exception) {
            return new long[]{-1L};
        }
    }
    
    // Dynamic address structure promotion mapped securely to guest memory buffers
    private static long[] doRecv(
        @NotNull Instance instance,
        int socketFileDescriptor,
        int destinationBufferPointer,
        int desinationBufferLength,
        @SuppressWarnings("unused") int flags,
        int sourceAddressPointer,
        int sourceAddressLength
    ) {
        @Nullable SocketHandle handle = sockets.get(socketFileDescriptor);
        if (handle == null) return new long[]{-1L};
        
        // POSIX ABI: if src_addr != NULL, addrlen must be a valid value-result pointer.
        if (sourceAddressPointer != 0 && sourceAddressLength == 0) {
            return new long[]{-1L};
        }
        
        try {
            @NotNull Memory memory = instance.memory();
            @NotNull ByteBuffer buffer = ByteBuffer.allocate(desinationBufferLength);
            
            if (handle.isUdp && handle.datagramChannel != null) {
                SocketAddress sender = handle.datagramChannel.receive(buffer);
                if (sender == null) {
                    return new long[]{-1L};
                }
                
                buffer.flip();
                memory.write(destinationBufferPointer, Arrays.copyOf(buffer.array(), buffer.limit()));
                
                if (sourceAddressPointer != 0) {
                    if (!(sender instanceof InetSocketAddress) ||
                        writeSockaddrResult(memory, sourceAddressPointer, sourceAddressLength, (InetSocketAddress) sender, handle.addressFamily)) {
                        return new long[]{-1L};
                    }
                }
                
                return new long[]{buffer.limit()};
            } else if (handle.socketChannel != null) {
                int read = handle.socketChannel.read(buffer);
                if (read > 0) {
                    memory.write(destinationBufferPointer, Arrays.copyOf(buffer.array(), read));
                    return new long[]{read};
                }
                return new long[]{read == -1 ? 0 : -1L};
            }
            
            return new long[]{-1L};
        } catch (@NotNull IOException exception) {
            return new long[]{-1L};
        }
    }
    
    private static long[] closeHandle(int handleID) {
        @Nullable SocketHandle handle = sockets.remove(handleID);
        if (handle != null) {
            try {
                if (handle.serverChannel != null) handle.serverChannel.close();
            } catch (@NotNull IOException ignored) {
            }
            try {
                if (handle.datagramChannel != null) handle.datagramChannel.close();
            } catch (@NotNull IOException ignored) {
            }
            try {
                if (handle.socketChannel != null) handle.socketChannel.close();
            } catch (@NotNull IOException ignored) {
            }
        }
        return new long[]{0};
    }
    
    private static @NotNull List<@NotNull ValueType> params(int count) {
        @NotNull List<@NotNull ValueType> res = new ArrayList<>(count);
        for (int i = 0; i < count; i++) res.add(ValueType.I32);
        return res;
    }
    
    private static @NotNull List<@NotNull ValueType> returns(int count) {
        return count == 0 ? List.of() : List.of(ValueType.I32);
    }
    
    private static void writeIntLE(@NotNull Memory mem, int offset, int val) {
        mem.write(offset, new byte[]{
            (byte) val, (byte) (val >> 8), (byte) (val >> 16), (byte) (val >> 24)
        });
    }
    
    private static int readIntLE(@NotNull Memory mem, int offset) {
        byte[] b = mem.readBytes(offset, 4);
        return (b[0] & 0xFF) | ((b[1] & 0xFF) << 8) | ((b[2] & 0xFF) << 16) | ((b[3] & 0xFF) << 24);
    }
    
    private static long readLongLE(@NotNull Memory mem, int offset) {
        byte[] b = mem.readBytes(offset, 8);
        long res = 0;
        for (int i = 0; i < 8; i++) {
            res |= ((long) (b[i] & 0xFF) << (8 * i));
        }
        return res;
    }
    
    private static @NotNull String readNullTerminatedString(@NotNull Memory mem, int offset) {
        ByteArrayOutputStream baos = new ByteArrayOutputStream();
        int curr = offset;
        while (true) {
            byte[] b = mem.readBytes(curr++, 1);
            if (b[0] == 0) break;
            baos.write(b[0]);
        }
        return baos.toString(StandardCharsets.UTF_8);
    }
    
    private record SockProtoPair(int socketType, int protocol) {
    }
    
    private record AddrInfoResult(
        int addressFamily,
        int socketType,
        int protocol,
        byte @NotNull [] socketAddress,
        @NotNull String canonicalName
    ) {
    }
    
    private static @Nullable ExportFunction findExport(@NotNull Instance instance, @NotNull String primary, @NotNull String fallback) {
        @Nullable ExportFunction function = instance.export(primary);
        return function != null ? function : instance.export(fallback);
    }
    
    private static int guestMalloc(@NotNull ExportFunction malloc, int size) {
        return (int) malloc.apply(size)[0];
    }
    
    private static void putU16LE(byte[] buffer, @SuppressWarnings("SameParameterValue") int offset, int value) {
        buffer[offset] = (byte) (value & 0xFF);
        buffer[offset + 1] = (byte) ((value >>> 8) & 0xFF);
    }
    
    private static void putU16BE(byte[] buffer, @SuppressWarnings("SameParameterValue") int offset, int value) {
        buffer[offset] = (byte) ((value >>> 8) & 0xFF);
        buffer[offset + 1] = (byte) (value & 0xFF);
    }
    
    private static void putIntLE(byte[] buffer, int offset, int value) {
        buffer[offset] = (byte) (value & 0xFF);
        buffer[offset + 1] = (byte) ((value >>> 8) & 0xFF);
        buffer[offset + 2] = (byte) ((value >>> 16) & 0xFF);
        buffer[offset + 3] = (byte) ((value >>> 24) & 0xFF);
    }
    
    private static byte @NotNull [] ipv4ToMappedIpv6(byte[] address) {
        byte[] returnValue = new byte[16];
        returnValue[10] = (byte) 0xFF;
        returnValue[11] = (byte) 0xFF;
        System.arraycopy(address, 0, returnValue, 12, 4);
        return returnValue;
    }
    
    private static boolean isIpv4MappedIpv6(byte[] address) {
        if (address == null || address.length != 16) return false;
        for (int index = 0; index < 10; index++) {
            if (address[index] != 0) return false;
        }
        return address[10] == (byte) 0xFF && address[11] == (byte) 0xFF;
    }
    
    private static byte @NotNull [] extractMappedIpv4(byte @NotNull [] address) {
        return Arrays.copyOfRange(address, 12, 16);
    }
    
    private static byte @NotNull [] encodeSockaddr(int addressFamily, byte @NotNull [] address, int port, int scopeId) {
        if (addressFamily == AF_INET) {
            if (address.length != 4) throw new IllegalArgumentException("AF_INET requires 4 bytes");
            byte @NotNull [] out = new byte[SOCKADDR_IN_LEN];
            putU16LE(out, 0, AF_INET);
            putU16BE(out, 2, port);
            System.arraycopy(address, 0, out, 4, 4);
            return out;
        }
        
        if (addressFamily == AF_INET6) {
            if (address.length != 16) throw new IllegalArgumentException("AF_INET6 requires 16 bytes");
            byte @NotNull [] out = new byte[SOCKADDR_IN6_LEN];
            putU16LE(out, 0, AF_INET6);
            putU16BE(out, 2, port);
            putIntLE(out, 4, 0); // flowinfo
            System.arraycopy(address, 0, out, 8, 16);
            putIntLE(out, 24, scopeId);
            return out;
        }
        
        throw new IllegalArgumentException("Unsupported address family '" + addressFamily + "'");
    }
    
    private static boolean writeSockaddrResult(
        @NotNull Memory memory,
        int addressPointer,
        int addressLength,
        @NotNull InetSocketAddress remoteAddress,
        int socketFamily
    ) {
        if (addressPointer == 0) {
            return false;
        }
        if (addressLength == 0) {
            return true;
        }
        
        @NotNull InetAddress inet = remoteAddress.getAddress();
        if (inet == null) {
            return true;
        }
        
        byte[] rawAddress = inet.getAddress();
        int addressFamily;
        int scopeId = 0;
        
        if (socketFamily == AF_INET6) {
            addressFamily = AF_INET6;
            if (rawAddress.length == 4) {
                rawAddress = ipv4ToMappedIpv6(rawAddress);
            }
            if (inet instanceof Inet6Address) {
                scopeId = ((Inet6Address) inet).getScopeId();
            }
        } else {
            addressFamily = AF_INET;
            if (rawAddress.length == 16) {
                if (!isIpv4MappedIpv6(rawAddress)) {
                    return true;
                }
                rawAddress = extractMappedIpv4(rawAddress);
            }
        }
        
        byte @NotNull [] socketAddress = encodeSockaddr(addressFamily, rawAddress, remoteAddress.getPort(), scopeId);
        int callerLength = Math.max(0, readIntLE(memory, addressLength));
        int copyLength = Math.min(callerLength, socketAddress.length);
        
        if (copyLength > 0) {
            memory.write(addressPointer, Arrays.copyOf(socketAddress, copyLength));
        }
        writeIntLE(memory, addressLength, socketAddress.length);
        return false;
    }
    
    private static @Nullable Integer parseNumericService(@Nullable String string) {
        if (string == null || string.isEmpty()) return null;
        for (int i = 0; i < string.length(); i++) {
            if (!Character.isDigit(string.charAt(i))) return null;
        }
        try {
            int port = Integer.parseInt(string);
            return (port >= 0 && port <= 65535) ? port : null;
        } catch (NumberFormatException e) {
            return null;
        }
    }
    
    private static int hexValue(char character) {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return 10 + (character - 'a');
        if (character >= 'A' && character <= 'F') return 10 + (character - 'A');
        return -1;
    }
    
    private static byte[] parseIpv4Literal(@Nullable String string) {
        if (string == null || string.isEmpty()) return null;
        @NotNull String @NotNull [] parts = string.split("\\.", -1);
        if (parts.length != 4) return null;
        
        byte @NotNull [] returnValue = new byte[4];
        for (int index = 0; index < 4; index++) {
            @NotNull String part = parts[index];
            if (part.isEmpty() || part.length() > 3) return null;
            int value = 0;
            for (int partIndex = 0; partIndex < part.length(); partIndex++) {
                char character = part.charAt(partIndex);
                if (!Character.isDigit(character)) return null;
                value = (value * 10) + (character - '0');
            }
            if (value < 0 || value > 255) return null;
            returnValue[index] = (byte) value;
        }
        return returnValue;
    }
    
    private static @Nullable List<@NotNull Integer> parseIpv6Section(@NotNull String part, boolean allowIpv4Tail) {
        @NotNull List<@NotNull Integer> words = new ArrayList<>();
        if (part.isEmpty()) return words;
        
        String[] tokens = part.split(":", -1);
        for (int index = 0; index < tokens.length; index++) {
            @NotNull String token = tokens[index];
            if (token.isEmpty()) return null;
            
            if (token.indexOf('.') >= 0) {
                if (!allowIpv4Tail || index != tokens.length - 1) return null;
                byte @NotNull [] address = parseIpv4Literal(token);
                if (address == null) return null;
                words.add(((address[0] & 0xFF) << 8) | (address[1] & 0xFF));
                words.add(((address[2] & 0xFF) << 8) | (address[3] & 0xFF));
            } else {
                if (token.length() > 4) return null;
                int value = 0;
                for (int tokenIndex = 0; tokenIndex < token.length(); tokenIndex++) {
                    int hexValue = hexValue(token.charAt(tokenIndex));
                    if (hexValue < 0) return null;
                    value = (value << 4) | hexValue;
                }
                words.add(value);
            }
        }
        
        return words;
    }
    
    private static byte @Nullable [] parseIpv6Literal(@Nullable String string) {
        if (string == null || string.isEmpty()) return null;
        if (string.indexOf('%') >= 0) return null; // inet_pton does not accept zone ids
        
        @NotNull String @NotNull [] halves = string.split("::", -1);
        if (halves.length > 2) return null;
        
        @NotNull List<@NotNull Integer> words = new ArrayList<>(8);
        
        if (halves.length == 1) {
            @Nullable List<@NotNull Integer> all = parseIpv6Section(string, true);
            if (all == null || all.size() != 8) return null;
            words.addAll(all);
        } else {
            @Nullable List<@NotNull Integer> left = parseIpv6Section(halves[0], false);
            @Nullable List<@NotNull Integer> right = parseIpv6Section(halves[1], true);
            if (left == null || right == null) return null;
            
            int zeros = 8 - (left.size() + right.size());
            if (zeros < 1) return null;
            
            words.addAll(left);
            for (int i = 0; i < zeros; i++) words.add(0);
            words.addAll(right);
        }
        
        if (words.size() != 8) return null;
        
        byte[] returnValue = new byte[16];
        for (int index = 0; index < 8; index++) {
            int word = words.get(index);
            returnValue[index * 2] = (byte) ((word >>> 8) & 0xFF);
            returnValue[index * 2 + 1] = (byte) (word & 0xFF);
        }
        return returnValue;
    }
    
    private static @NotNull List<@NotNull InetAddress> defaultAddressesForNullNode(
        int addressFamily,
        boolean passive
    ) throws UnknownHostException {
        @NotNull List<@NotNull InetAddress> returnValue = new ArrayList<>();
        
        if (addressFamily == AF_UNSPEC || addressFamily == AF_INET6) {
            byte[] address = new byte[16];
            if (!passive) address[15] = 1; // ::1
            returnValue.add(InetAddress.getByAddress(address));
        }
        
        if (addressFamily == AF_UNSPEC || addressFamily == AF_INET) {
            byte[] address = passive ? new byte[]{0, 0, 0, 0} : new byte[]{127, 0, 0, 1};
            returnValue.add(InetAddress.getByAddress(address));
        }
        
        return returnValue;
    }
    
    private static @NotNull List<@NotNull InetAddress> resolveNumericNode(
        int addressFamily,
        int flags,
        byte @Nullable [] v4Address,
        byte @Nullable [] v6Address
    ) throws UnknownHostException {
        List<InetAddress> returnValue = new ArrayList<>();
        
        if (v4Address != null) {
            if (addressFamily == AF_UNSPEC || addressFamily == AF_INET) {
                returnValue.add(InetAddress.getByAddress(v4Address));
            } else if (addressFamily == AF_INET6 && (flags & AI_V4MAPPED) != 0) {
                returnValue.add(InetAddress.getByAddress(ipv4ToMappedIpv6(v4Address)));
            }
            return returnValue;
        }
        
        if (v6Address != null) {
            if (addressFamily == AF_UNSPEC || addressFamily == AF_INET6) {
                returnValue.add(InetAddress.getByAddress(v6Address));
            }
        }
        
        return returnValue;
    }
    
    private static @NotNull List<@NotNull InetAddress> resolveDnsNode(@NotNull String node, int addressFamily, int flags) throws UnknownHostException {
        InetAddress[] resolvedAddresses = InetAddress.getAllByName(node);
        
        List<InetAddress> v4Address = new ArrayList<>();
        List<InetAddress> v6Address = new ArrayList<>();
        for (InetAddress address : resolvedAddresses) {
            if (address.getAddress().length == 16) v6Address.add(address);
            else v4Address.add(address);
        }
        
        List<InetAddress> returnValue = new ArrayList<>();
        
        if (addressFamily == AF_UNSPEC) {
            returnValue.addAll(Arrays.asList(resolvedAddresses));
            return returnValue;
        }
        
        if (addressFamily == AF_INET) {
            returnValue.addAll(v4Address);
            return returnValue;
        }
        
        // AF_INET6
        returnValue.addAll(v6Address);
        if ((flags & AI_V4MAPPED) != 0 && (((flags & AI_ALL) != 0) || v6Address.isEmpty())) {
            for (@NotNull InetAddress a : v4Address) {
                returnValue.add(InetAddress.getByAddress(ipv4ToMappedIpv6(a.getAddress())));
            }
        }
        return returnValue;
    }
    
    private static @NotNull List<@NotNull SockProtoPair> buildSockProtoPairs(int socktype, int protocol) {
        @NotNull List<@NotNull SockProtoPair> returnValue = new ArrayList<>();
        
        if (socktype == 0) {
            if (protocol == 0 || protocol == IPPROTO_TCP) {
                returnValue.add(new SockProtoPair(SOCK_STREAM, IPPROTO_TCP));
            }
            if (protocol == 0 || protocol == IPPROTO_UDP) {
                returnValue.add(new SockProtoPair(SOCK_DGRAM, IPPROTO_UDP));
            }
            return returnValue;
        }
        
        if (socktype == SOCK_STREAM) {
            returnValue.add(new SockProtoPair(SOCK_STREAM, IPPROTO_TCP));
            return returnValue;
        }
        
        if (socktype == SOCK_DGRAM) {
            returnValue.add(new SockProtoPair(SOCK_DGRAM, IPPROTO_UDP));
            return returnValue;
        }
        
        return returnValue;
    }
}
