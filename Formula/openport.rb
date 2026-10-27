class Openport < Formula
  desc "Options analytics and paper-trading simulator"
  homepage "https://github.com/38st/openport"
  version "0.5.0"
  license "MIT"

  depends_on :macos

  on_arm do
    url "https://github.com/38st/openport/releases/download/v0.5.0/openport-0.5.0-darwin-arm64.tar.gz"
    sha256 "a1375f048b52faebb962946fb00d30b73338b546a10ca9873ac599178ed0a4cb"
  end

  on_intel do
    url "https://github.com/38st/openport.git", tag: "v0.5.0"
    depends_on "cmake" => :build
    depends_on "node" => :build
    depends_on "boost" => :build
    depends_on "openssl@3"
    depends_on "zstd"
  end

  def install
    if Hardware::CPU.intel?
      cd "web" do
        system "npm", "ci", "--no-audit", "--no-fund"
        system "npm", "run", "build"
      end
      system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
             "-DOPENPORT_BUILD_TESTS=OFF", "-DOPENPORT_BUILD_BENCHMARKS=OFF"
      system "cmake", "--build", "build", "-j3"
      system "cmake", "--install", "build", "--component", "openport"
    else
      bin.install Dir["bin/*"]
      (share/"openport").install "share/openport/web"
    end
    doc.install Dir["docs/*"], "README.md"
  end

  def post_install
    (var/"openport").mkpath
    (var/"log").mkpath
  end

  service do
    run [opt_bin/"openportd", "--address", "127.0.0.1",
         "--paper-journal", var/"openport/paper-journal.jsonl",
         "--candle-dir", var/"openport/candles",
         "--series-dir", var/"openport/series",
         "--write-token-file", var/"openport/write-token",
         "--web-root", opt_share/"openport/web"]
    keep_alive true
    working_dir var/"openport"
    log_path var/"log/openport.log"
    error_log_path var/"log/openport.error.log"
  end

  def caveats
    <<~EOS
      Start the terminal with:
        brew services start openport

      Open the running terminal without searching its log:
        openportd --print-url --open --write-token-file #{var}/openport/write-token
      Omit --open to print the link. If the service has not created its token yet,
      wait a few seconds and retry. For a foreground session, use openportd --open.
      The link saves the token in that browser tab for paper-trading writes.
      Accounts and chart history are kept under #{var}/openport.
      Protect the token file and logs; the link grants write access.

      Apple Silicon uses the release archive; Intel Macs build the release tag
      from source. For upgrades and backups, see docs/upgrade-and-backup.md.
    EOS
  end

  test do
    assert_equal "openportd #{version}\n", shell_output("#{bin}/openportd --version")
  end
end
