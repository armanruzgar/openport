class Openport < Formula
  desc "Options analytics and paper-trading simulator"
  homepage "https://github.com/armanruzgar/openport"
  version "0.5.1"
  license "MIT"

  depends_on :macos

  on_arm do
    # The release archive's programs need macOS 15 or later (OPENPORT_MACOS_FLOOR).
    depends_on macos: :sequoia
    url "https://github.com/armanruzgar/openport/releases/download/v0.5.1/openport-0.5.1-darwin-arm64.tar.gz"
    sha256 "2757841951a36a24c6d90f0ba1e9ae018b5440fc5d3f3366cceb634be733712c"
  end

  on_intel do
    url "https://github.com/armanruzgar/openport.git", tag: "v0.5.1"
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
    doc.install "THIRD_PARTY_NOTICES.txt" if File.exist?("THIRD_PARTY_NOTICES.txt")
  end

  def post_install
    (var/"openport").mkpath
    # Accounts, imported holdings and the write token: the owner's alone.
    (var/"openport").chmod 0700
    (var/"log").mkpath
    (etc/"openport").mkpath
  end

  service do
    # openportd with the data paths below, then the settings in etc/openport, which
    # upgrades keep: openportd.args holds flags, one per line with its value after a
    # blank, and openportd.env NAME=VALUE lines, such as provider keys, which launchd
    # does not take from a shell. Blank lines and # comments are skipped. The script is
    # the command itself, so it needs nothing installed beside openportd.
    run ["/bin/sh", "-c", <<~SH, "openport-service"]
      settings="#{etc}/openport"
      number=0
      if [ -f "$settings/openportd.env" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
          number=$((number + 1))
          line="${line#"${line%%[![:blank:]]*}"}"
          line="${line%"${line##*[![:blank:]]}"}"
          case "$line" in ''|'#'*) continue ;; esac
          name="${line%%=*}"
          case "$name" in
            "$line"|''|[0-9]*|*[!A-Za-z0-9_]*)
              echo "$0: $settings/openportd.env line $number: expected NAME=VALUE" >&2
              exit 2 ;;
          esac
          export "$name=${line#*=}"
        done < "$settings/openportd.env"
      fi
      set --
      if [ -f "$settings/openportd.args" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
          line="${line#"${line%%[![:blank:]]*}"}"
          line="${line%"${line##*[![:blank:]]}"}"
          case "$line" in
            ''|'#'*) ;;
            *[[:blank:]]*)
              value="${line#*[[:blank:]]}"
              set -- "$@" "${line%%[[:blank:]]*}" "${value#"${value%%[![:blank:]]*}"}" ;;
            *) set -- "$@" "$line" ;;
          esac
        done < "$settings/openportd.args"
      fi
      exec "#{opt_bin}/openportd" --address 127.0.0.1 --no-token-link \\
        --paper-journal "#{var}/openport/paper-journal.jsonl" \\
        --candle-dir "#{var}/openport/candles" \\
        --series-dir "#{var}/openport/series" \\
        --write-token-file "#{var}/openport/write-token" \\
        --web-root "#{opt_pkgshare}/web" "$@"
    SH
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
      The link grants write access: keep it and the token file private. The
      service's log, which other local accounts can read, shows that command
      instead of the link.

      The service watches Cboe's delayed SPX, SPY, QQQ, IWM and DIA. Other flags,
      such as --provider, --symbols, --record-dir or --notify-config, go in
        #{etc}/openport/openportd.args
      one per line with its value after a space; provider keys and OPENPORT_NOTIFY_*
      settings go in #{etc}/openport/openportd.env as NAME=VALUE
      lines (chmod 600 it). Then: brew services restart openport

      Apple Silicon uses the release archive; Intel Macs build the release tag
      from source. For upgrades and backups, see docs/upgrade-and-backup.md.
    EOS
  end

  test do
    assert_equal "openportd #{version}\n", shell_output("#{bin}/openportd --version")
  end
end
