package framework

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"

	"go.uber.org/zap"
)

// QEMUManager handles the complete lifecycle and operations of QEMU virtual machines
// for YANET functional testing. It provides comprehensive VM management including
// startup, networking configuration, filesystem sharing, and graceful shutdown.
//
// The manager supports:
//   - QEMU VM lifecycle management with proper resource cleanup
//   - Unix socket-based networking for packet injection and capture
//   - 9P filesystem sharing for host-VM file exchange
//   - Serial console and monitor interface access
//   - VM readiness detection and synchronization
//   - Parallel test execution with unique instance isolation
//
// All operations are thread-safe and support concurrent access patterns
// required for comprehensive network testing scenarios.
type QEMUManager struct {
	ImagePath   string             // Path to the QEMU disk image file
	WorkDir     string             // Temporary working directory for VM instance
	Command     *exec.Cmd          // QEMU process command handle
	BinariesDir string             // Directory for shared binary files
	ConfigDir   string             // Directory for configuration files
	BuildDir    string             // Project build directory (shared with VM)
	TargetDir   string             // Project target directory (shared with VM)
	SerialPath  string             // Unix socket path for serial console access
	MonitorPath string             // Unix socket path for QEMU monitor interface
	SocketPaths []string           // Unix socket paths for network interfaces
	isReady     bool               // VM readiness state flag
	readySignal chan bool          // Channel for VM readiness notification
	monitorConn net.Conn           // Connection to QEMU monitor interface
	serialConn  net.Conn           // Connection to VM serial console
	log         *zap.SugaredLogger // Logger for debugging and monitoring
	readyMutex  sync.RWMutex       // Protects concurrent access to isReady field
	instanceID  string             // Unique identifier for this VM instance
}

// NewQEMUManager creates and initializes a new QEMU manager instance for virtual
// machine testing. The manager sets up all necessary directories, generates unique
// instance identifiers for parallel execution, and configures filesystem sharing
// paths for host-VM communication.
//
// The initialization process includes:
//   - Unique instance ID generation for parallel test isolation
//   - Working directory creation in system temporary space
//   - Project root detection for build and target directory sharing
//   - Socket path configuration for VM networking and console access
//
// Parameters:
//   - imagePath: Path to the QEMU disk image file (must exist and be accessible)
//   - logger: Structured logger for debugging and monitoring VM operations
//
// Returns:
//   - *QEMUManager: Configured QEMU manager ready for VM startup
//   - error: An error if project root detection fails or paths are invalid
//
// Example:
//
//	manager, err := NewQEMUManager("/path/to/vm-image.qcow2", logger)
//	if err != nil {
//	    log.Fatalf("Failed to create QEMU manager: %v", err)
//	}
func NewQEMUManager(imagePath string, logger *zap.SugaredLogger) (*QEMUManager, error) {
	// Generate unique instance ID for parallel execution
	instanceID := fmt.Sprintf("yanet-vm-%d-%d", os.Getpid(), time.Now().UnixNano())
	workDir := filepath.Join(os.TempDir(), instanceID)

	// Determine project root directory
	projectRoot, err := findProjectRoot()
	if err != nil {
		return nil, fmt.Errorf("failed to determine project root directory: %w", err)
	}
	buildDir := filepath.Join(projectRoot, "build")
	targetDir := filepath.Join(projectRoot, "target")

	return &QEMUManager{
		ImagePath:   imagePath,
		WorkDir:     workDir,
		BinariesDir: filepath.Join(workDir, "bin"),
		ConfigDir:   filepath.Join(workDir, "config"),
		BuildDir:    buildDir,
		TargetDir:   targetDir,
		readySignal: make(chan bool, 1),
		log:         logger,
		instanceID:  instanceID,
		SerialPath:  filepath.Join(workDir, "serial.sock"),
		MonitorPath: filepath.Join(workDir, "monitor.sock"),
	}, nil
}

// Start launches a QEMU virtual machine with comprehensive configuration for
// YANET testing including networking, filesystem sharing, and console access.
// This method handles the complete VM startup process with proper error handling
// and resource management.
//
// The startup process includes:
//   - QEMU binary availability verification
//   - VM image file existence validation
//   - Working directory structure creation
//   - Network interface configuration with Unix sockets
//   - 9P filesystem sharing setup for host-VM file exchange
//   - Serial console and monitor interface initialization
//   - VM process launch with proper logging and error capture
//   - Connection establishment to VM interfaces
//   - Background VM readiness monitoring
//
// Network Configuration:
//   - User networking for internet access
//   - Two virtio-net interfaces with Unix socket backends
//   - IOMMU and modern virtio features enabled for performance
//
// Filesystem Sharing:
//   - Binaries directory for executable sharing
//   - Configuration directory for runtime config files
//   - Build directory for YANET binaries access
//   - Target directory for build artifacts
//   - Complete project directory for source code access
//
// Returns:
//   - error: An error if VM startup fails, networking cannot be configured,
//     or console connections cannot be established
//
// Example:
//
//	if err := manager.Start(); err != nil {
//	    log.Fatalf("VM startup failed: %v", err)
//	}
func (q *QEMUManager) Start() error {
	// Check if QEMU is available
	if _, err := exec.LookPath("qemu-system-x86_64"); err != nil {
		return fmt.Errorf("qemu-system-x86_64 not found in PATH: %w", err)
	}

	// Check if image file exists
	if _, err := os.Stat(q.ImagePath); err != nil {
		return fmt.Errorf("QEMU image %s not found: %w", q.ImagePath, err)
	}

	// Create working directories
	q.log.Debug("Creating binaries directory...")
	if err := os.MkdirAll(q.BinariesDir, 0755); err != nil {
		return fmt.Errorf("failed to create binaries directory: %w", err)
	}
	q.log.Debug("Binaries directory created.")

	q.log.Debug("Creating config directory...")
	if err := os.MkdirAll(q.ConfigDir, 0755); err != nil {
		return fmt.Errorf("failed to create config directory: %w", err)
	}
	q.log.Debug("Config directory created.")

	// Generate socket paths for Unix stream interface
	q.log.Debug("Generating socket paths...")
	q.SocketPaths = make([]string, 2) // Assuming 2 interfaces for now
	for i := range q.SocketPaths {
		// Use /tmp/ directory like in working Makefile configuration
		q.SocketPaths[i] = filepath.Join("/tmp", fmt.Sprintf("yanetvm_%s_sockdev_%d.sock", q.instanceID, i))
	}
	q.log.Debug("Socket paths generated.")

	// Detect OS
	osType := runtime.GOOS

	// Base arguments
	args := []string{
		"-name", "yanet-test-vm",
		"-smp", "2",
		"-m", "5G",
		"-machine", "q35,kernel-irqchip=split",
		"-cpu", "max",
		"-snapshot",
		"-device", "intel-iommu,intremap=on,device-iotlb=on",
		"-device", "ioh3420,id=pcie.1,chassis=1",
		"-device", "ioh3420,id=pcie.2,chassis=2",
	}

	// OS-specific configuration
	if osType == "linux" {
		if isKVMEnabled() {
			args = append(args, "-enable-kvm")
		}
	}

	// Drive configuration
	args = append(args,
		"-drive", fmt.Sprintf("file=%s,if=virtio,format=qcow2", q.ImagePath),
	)
	// Network interface configuration
	args = append(args,
		"-netdev", "user,id=net0",
		"-device", "virtio-net-pci,netdev=net0,mac=AA:BB:CC:DD:CA:B0",
		"-netdev", "stream,id=net1,server=on,addr.type=unix,addr.path="+q.SocketPaths[0],
		"-device", "virtio-net-pci,bus=pcie.1,netdev=net1,mac=52:54:00:6b:ff:a5,disable-legacy=on,disable-modern=off,iommu_platform=on,ats=on,vectors=10",
		"-netdev", "stream,id=net2,server=on,addr.type=unix,addr.path="+q.SocketPaths[1],
		"-device", "virtio-net-pci,bus=pcie.2,netdev=net2,mac=52:54:00:11:00:03,disable-legacy=on,disable-modern=off,iommu_platform=on,ats=on,vectors=10",
	)

	// Add 9P filesystem sharing for YANET binaries and configuration
	// This allows the VM to access host files for testing
	// Match the mount configuration used in Makefile
	args = append(args,
		// Share temporary directory for binaries
		"-fsdev", "local,id=fsdev0,path="+q.BinariesDir+",security_model=none",
		"-device", "virtio-9p-pci,fsdev=fsdev0,mount_tag=binaries",
		// Share temporary directory for configuration
		"-fsdev", "local,id=fsdev1,path="+q.ConfigDir+",security_model=none",
		"-device", "virtio-9p-pci,fsdev=fsdev1,mount_tag=config",
		// Share build directory
		//"-fsdev", "local,id=fsdev2,path="+q.BuildDir+",security_model=none,readonly=on",
		"-fsdev", "local,id=fsdev2,path="+q.BuildDir+",security_model=none",
		"-device", "virtio-9p-pci,fsdev=fsdev2,mount_tag=build",
		// Share target directory
		"-fsdev", "local,id=fsdev3,path="+q.TargetDir+",security_model=none,readonly=on",
		"-device", "virtio-9p-pci,fsdev=fsdev3,mount_tag=target",
		// Share all code directory
		"-fsdev", "local,id=fsdev4,path="+q.TargetDir+"/..,security_model=none,readonly=on",
		"-device", "virtio-9p-pci,fsdev=fsdev4,mount_tag=yanet2",
	)

	qemuLogfile := filepath.Join(q.WorkDir, "yanet-test-vm.log")
	// Logging and display options - using unix sockets
	args = append(args,
		"-D", qemuLogfile,
		"-serial", fmt.Sprintf("unix:%s,server=on", q.SerialPath),
		"-monitor", fmt.Sprintf("unix:%s,server=on", q.MonitorPath),
		"-display", "none",
		"-no-reboot",
	)
	// Create log file for QEMU output
	logFile := filepath.Join(q.WorkDir, "qemu-output.log")
	logWriter, err := os.Create(logFile)
	if err != nil {
		return fmt.Errorf("failed to create log file: %w", err)
	}
	defer func() {
		if err := logWriter.Close(); err != nil {
			q.log.Errorf("Failed to close log file: %v", err)
		}
	}()

	// Start QEMU
	q.Command = exec.Command("qemu-system-x86_64", args...)

	q.log.Debugf("Starting QEMU with command: %s %s", q.Command.Path, strings.Join(args, " "))
	q.log.Debugf("QEMU logs will be written to: %s", logFile)

	// Create stderr pipe for logging
	stderr, err := q.Command.StderrPipe()
	if err != nil {
		return fmt.Errorf("failed to create stderr pipe: %w", err)
	}

	// Start goroutine to capture stderr for logging
	go q.captureStderr(stderr, logWriter)

	// Start QEMU process
	if err := q.Command.Start(); err != nil {
		return fmt.Errorf("failed to start QEMU: %w", err)
	}

	// Wait a bit and check if process is still running
	time.Sleep(1 * time.Second)

	// Check if process exited with error
	if q.Command.Process == nil || q.Command.Process.Pid == 0 || (q.Command.ProcessState != nil && q.Command.ProcessState.Exited()) {
		// Read log file to see what went wrong
		logContent, _ := os.ReadFile(logFile)
		logContentQ, _ := os.ReadFile(qemuLogfile)
		return fmt.Errorf("QEMU exited early. Log content: %s; QEMU log content: %s", string(logContent), string(logContentQ))
	}

	p, err := os.FindProcess(q.Command.Process.Pid)
	if err != nil || p == nil {
		logContent, _ := os.ReadFile(logFile)
		logContentQ, _ := os.ReadFile(qemuLogfile)
		return fmt.Errorf("QEMU proccess not found. Log content: %s; QEMU log content: %s", string(logContent), string(logContentQ))
	}
	if err := p.Signal(syscall.Signal(0)); err != nil {
		logContent, _ := os.ReadFile(logFile)
		logContentQ, _ := os.ReadFile(qemuLogfile)
		return fmt.Errorf("failed to signal QEMU process(may be dead): %w. Log content: %s; QEMU log content: %s", err, string(logContent), string(logContentQ))
	}

	q.log.Debugf("QEMU process started with PID: %d", q.Command.Process.Pid)
	q.log.Debugf("QEMU process state: %v", q.Command.ProcessState)

	if err := q.connectToMonitor(); err != nil {
		logContent, _ := os.ReadFile(logFile)
		logContentQ, _ := os.ReadFile(qemuLogfile)
		return fmt.Errorf("failed to connect to monitor (may be dead): %w. Log content: %s; QEMU log content: %s", err, string(logContent), string(logContentQ))
	}
	q.log.Debugf("Successfully connected to monitor at %s", q.MonitorPath)

	// Connect to serial console via unix socket
	if err := q.connectToSerial(); err != nil {
		return fmt.Errorf("failed to connect to serial console: %w", err)
	}
	q.log.Debugf("Successfully connected to serial console at %s", q.SerialPath)

	go q.monitorVMReadiness()

	return nil
}

// Stop performs graceful termination of the QEMU virtual machine and comprehensive
// cleanup of all associated resources. This method ensures proper resource
// deallocation and prevents resource leaks in testing environments.
//
// The cleanup process includes:
//   - Graceful closure of monitor and serial console connections
//   - QEMU process termination with proper signal handling
//   - Working directory and temporary file cleanup
//   - Unix socket file removal from filesystem
//   - Error collection and reporting for failed cleanup operations
//
// Multiple cleanup errors are collected and returned as a combined error
// to provide comprehensive information about any cleanup failures.
//
// Returns:
//   - error: A combined error if any cleanup operations fail, or nil if successful
//
// Example:
//
//	if err := manager.Stop(); err != nil {
//	    log.Errorf("VM cleanup encountered errors: %v", err)
//	}
func (q *QEMUManager) Stop() error {
	var errs []error

	// Close connections first (avoid double closing)
	if q.monitorConn != nil {
		if err := q.monitorConn.Close(); err != nil {
			errs = append(errs, fmt.Errorf("failed to close monitor connection: %w", err))
		}
		q.monitorConn = nil
	}
	if q.serialConn != nil {
		if err := q.serialConn.Close(); err != nil {
			errs = append(errs, fmt.Errorf("failed to close serial connection: %w", err))
		}
		q.serialConn = nil
	}

	// Kill QEMU process if running
	if q.Command != nil && q.Command.Process != nil {
		if err := q.Command.Process.Kill(); err != nil {
			errs = append(errs, fmt.Errorf("failed to kill QEMU process: %w", err))
		}
	}

	// Cleanup working directory
	if err := os.RemoveAll(q.WorkDir); err != nil {
		errs = append(errs, fmt.Errorf("failed to cleanup working directory: %w", err))
	}

	for _, path := range q.SocketPaths {
		if err := os.Remove(path); err != nil {
			errs = append(errs, fmt.Errorf("failed to remove socket file: %w", err))
		}
	}

	if len(errs) > 0 {
		return fmt.Errorf("errors during cleanup: %v", errs)
	}
	return nil
}

// GetStdin returns the stdin pipe for the QEMU process
func (q *QEMUManager) GetStdin() io.WriteCloser {
	// Try to connect if not already connected
	if q.serialConn == nil {
		if err := q.connectToSerial(); err != nil {
			q.log.Errorf("Failed to connect to serial console: %v", err)
			return nil
		}
	}
	return q.serialConn
}

// GetStdout returns the stdout pipe for the QEMU process
func (q *QEMUManager) GetStdout() io.ReadCloser {
	// Try to connect if not already connected
	if q.serialConn == nil {
		if err := q.connectToSerial(); err != nil {
			q.log.Errorf("Failed to connect to serial console: %v", err)
			return nil
		}
	}
	return q.serialConn
}

// captureStderr captures QEMU stderr for logging
func (q *QEMUManager) captureStderr(stderr io.ReadCloser, logWriter *os.File) {
	go func() {
		defer func() {
			if err := stderr.Close(); err != nil {
				q.log.Errorf("Failed to close stderr pipe: %v", err)
			}
		}()
		scanner := bufio.NewScanner(stderr)
		for scanner.Scan() {
			line := scanner.Text()
			if _, err := logWriter.WriteString("STDERR: " + line + "\n"); err != nil {
				q.log.Errorf("Failed to write stderr to log file: %v", err)
			}
		}
		if err := scanner.Err(); err != nil {
			q.log.Errorf("Error reading stderr: %v", err)
		}
	}()
}

// connectToMonitor connects to QEMU monitor interface via unix socket
func (q *QEMUManager) connectToMonitor() error {
	// Try multiple times to connect to monitor
	for i := 0; i < 10; i++ {
		// Create connection for monitoring
		conn, err := net.Dial("unix", q.MonitorPath)
		if err != nil {
			q.log.Debugf("Monitor connection attempt %d to %s failed: %v", i+1, q.MonitorPath, err)
			time.Sleep(1 * time.Second)
			continue
		}

		// Use connection
		q.monitorConn = conn
		q.log.Debugf("Successfully connected to monitor at %s", q.MonitorPath)

		return nil
	}

	return fmt.Errorf("failed to connect to monitor after 10 attempts")
}

// connectToSerial connects to QEMU serial console via unix socket
func (q *QEMUManager) connectToSerial() error {
	// Try multiple times to connect to serial console
	for i := 0; i < 10; i++ {
		// Create connection for commands
		conn, err := net.Dial("unix", q.SerialPath)
		if err != nil {
			q.log.Debugf("Serial connection attempt %d to %s failed: %v", i+1, q.SerialPath, err)
			time.Sleep(1 * time.Second)
			continue
		}

		// Use connection
		q.serialConn = conn
		q.log.Debugf("Successfully connected to serial console at %s", q.SerialPath)

		return nil
	}

	return fmt.Errorf("failed to connect to serial console after 10 attempts")
}

// monitorVMReadiness monitors serial console output to detect when VM is ready
func (q *QEMUManager) monitorVMReadiness() {
	if q.serialConn == nil {
		q.log.Error("Failed to monitor VM readiness: serial connection is nil")
		return
	}

	scanner := bufio.NewScanner(q.serialConn)

	for scanner.Scan() {
		line := scanner.Text()
		q.log.Debugf("VM output: %s", line)

		// If we see the unminimize message, send Enter to activate prompt
		if strings.Contains(line, "To restore this content, you can run the 'unminimize' command") {
			q.log.Debug("Unminimize message seen, sending Enter to activate prompt")
			if q.serialConn != nil {
				if _, err := q.serialConn.Write([]byte("\n")); err != nil {
					q.log.Errorf("Failed to send Enter to serial console: %v", err)
				}
			}
		}

		// Check if VM is ready - look for shell prompt
		if strings.Contains(line, "root@yanet-vm:~#") {
			q.setVMReady(true)
			q.log.Debug("VM is ready!")
			close(q.readySignal)
			return
		}
	}

	// Check for scanner errors
	if err := scanner.Err(); err != nil {
		q.log.Errorf("Error reading from serial console: %v", err)
	}
}

// WaitForReady blocks until the virtual machine becomes ready for command
// execution or the specified timeout expires. This method provides synchronous
// waiting for VM readiness with proper timeout handling.
//
// The method first checks if the VM is already ready to avoid unnecessary
// waiting. If not ready, it waits for the readiness signal from the background
// monitoring goroutine.
//
// Parameters:
//   - timeout: Maximum time to wait for VM readiness
//
// Returns:
//   - error: An error if the timeout expires before VM becomes ready, or nil if ready
//
// Example:
//
//	if err := manager.WaitForReady(60 * time.Second); err != nil {
//	    log.Fatalf("VM failed to become ready: %v", err)
//	}
func (q *QEMUManager) WaitForReady(timeout time.Duration) error {
	if q.IsVMReady() {
		q.log.Debug("VM is already ready")
		return nil
	}

	select {
	case <-q.readySignal:
		q.log.Debug("Got ready signal")
		return nil
	case <-time.After(timeout):
		return fmt.Errorf("VM did not become ready within %v", timeout)
	}
}

// IsVMReady returns the current readiness state of the virtual machine in a
// thread-safe manner. This method can be called from multiple goroutines
// without synchronization concerns.
//
// VM readiness indicates that the virtual machine has completed its boot
// process and is ready to accept and execute commands through the serial console.
//
// Returns:
//   - bool: True if the VM is ready for command execution, false otherwise
//
// Example:
//
//	if manager.IsVMReady() {
//	    // Safe to execute commands
//	    output, err := cli.ExecuteCommand("ls -la")
//	}
func (q *QEMUManager) IsVMReady() bool {
	q.readyMutex.RLock()
	defer q.readyMutex.RUnlock()
	return q.isReady
}

// setVMReady updates the VM readiness state in a thread-safe manner. This method
// is used internally by the readiness monitoring goroutine to update the VM
// state when readiness conditions are detected.
//
// The method uses a write lock to ensure exclusive access during state updates
// and prevent race conditions with concurrent readiness checks.
//
// Parameters:
//   - ready: New readiness state to set (true when VM is ready, false otherwise)
func (q *QEMUManager) setVMReady(ready bool) {
	q.readyMutex.Lock()
	defer q.readyMutex.Unlock()
	q.isReady = ready
}

// isKVMEnabled checks if KVM is available on the system.
func isKVMEnabled() bool {
	if _, err := os.Stat("/dev/kvm"); err == nil {
		return true
	}
	return false
}
