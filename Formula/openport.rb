class Openport < Formula
  desc "Options analytics and paper-trading simulator"
  homepage "https://github.com/38st/openport"
  url "https://github.com/38st/openport/releases/download/v0.4.0/openport-0.4.0-darwin-arm64.tar.gz"
  version "0.4.0"
  sha256 "e372f5f6430cf9ed967a80f69952c041cc016843781b1df820d2964b7e12c23a"
  license "MIT"

  depends_on :macos
  depends_on arch: :arm64

  def install
    bin.install Dir["bin/*"]
    (share/"openport").install "share/openport/web"
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

      Open the http://localhost:8080/#token=... link in #{var}/log/openport.log.
      The token is kept in #{var}/openport/write-token. If the log is buffered,
      open this URL with the contents of that file after #token=:
        http://localhost:8080/#token=TOKEN
      The link saves the token in that browser tab for paper-trading writes.
      Accounts and chart history are kept under #{var}/openport.
      Protect the token file and logs; the link grants write access.

      This formula installs the Apple Silicon archive. Use Docker on Intel Macs
      and Linux; see the README for Docker Compose and source builds.
    EOS
  end

  test do
    assert_equal "openportd #{version}\n", shell_output("#{bin}/openportd --version")
  end
end
