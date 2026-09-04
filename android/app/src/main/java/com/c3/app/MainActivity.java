package com.c3.app;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Locale;
import java.util.UUID;

public class MainActivity extends AppCompatActivity {

    private static final String DEV_PREFIX = "11C3";
    private static final UUID SVC_CTRL = UUID.fromString("c3a50001-3e74-4b1e-b6f4-1a91f7080001");
    private static final UUID CHR_CTRL = UUID.fromString("c3a50002-3e74-4b1e-b6f4-1a91f7080002");
    private static final UUID CHR_UPLOAD = UUID.fromString("c3a50003-3e74-4b1e-b6f4-1a91f7080003");
    private static final UUID CHR_STATUS = UUID.fromString("c3a50004-3e74-4b1e-b6f4-1a91f7080004");

    private static final String[] FUNCS = {"常亮", "1Hz 闪", "2Hz 闪", "4Hz 闪"};
    private static final String[] PALETTE_NAMES = {
            "天依蓝", "浅天蓝", "深海蓝", "纯白",
            "薄荷绿", "暖黄", "樱粉", "薰衣紫"
    };
    private static final String[] PALETTE = {
            "#66CCFF", "#A6E3FF", "#2E9BD6", "#FFFFFF",
            "#7BE0C8", "#FFD166", "#F08CA4", "#B28DFF"
    };

    private static final int REQ_PICK_CSV = 200;

    private BluetoothManager btManager;
    private BluetoothAdapter btAdapter;
    private BluetoothLeScanner btScanner;
    private BluetoothGatt gatt;
    private BluetoothDevice targetDevice;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ArrayList<BluetoothDevice> foundDevices = new ArrayList<>();

    private TextView connStatus, deviceName, brightValue, csvName, log;
    private Button btnScan, btnConnect, btnPlay, btnStop, btnNext, btnPickCsv, btnUploadPlay;
    private SeekBar seekBright;
    private LinearLayout channelsContainer, paletteContainer;

    private final Channel[] channels = new Channel[10];
    private boolean connected = false;
    private boolean scanning = false;
    private boolean uploading = false;

    private Uri csvUri = null;
    private String csvContent = "";
    private String csvDisplayName = "";

    private static class Channel {
        int color = Color.rgb(102, 204, 255);
        int func = 0;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        connStatus = findViewById(R.id.conn_status);
        deviceName = findViewById(R.id.device_name);
        brightValue = findViewById(R.id.bright_value);
        csvName = findViewById(R.id.csv_name);
        log = findViewById(R.id.log);
        btnScan = findViewById(R.id.btn_scan);
        btnConnect = findViewById(R.id.btn_connect);
        btnPlay = findViewById(R.id.btn_play);
        btnStop = findViewById(R.id.btn_stop);
        btnNext = findViewById(R.id.btn_next);
        btnPickCsv = findViewById(R.id.btn_pick_csv);
        btnUploadPlay = findViewById(R.id.btn_upload_play);
        seekBright = findViewById(R.id.seek_bright);
        channelsContainer = findViewById(R.id.channels_container);
        paletteContainer = findViewById(R.id.palette_container);

        btManager = (BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
        if (btManager != null) btAdapter = btManager.getAdapter();
        if (btAdapter != null) btScanner = btAdapter.getBluetoothLeScanner();

        for (int i = 0; i < 10; i++) channels[i] = new Channel();
        buildChannelRows();
        buildPalette();

        btnScan.setOnClickListener(v -> startScan());
        btnConnect.setOnClickListener(v -> toggleConnect());
        btnPlay.setOnClickListener(v -> send("PLAY"));
        btnStop.setOnClickListener(v -> send("STOP"));
        btnNext.setOnClickListener(v -> send("NEXT"));
        btnPickCsv.setOnClickListener(v -> pickCsv());
        btnUploadPlay.setOnClickListener(v -> uploadAndPlay());

        seekBright.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int p, boolean f) {
                brightValue.setText(p + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar sb) {}
            @Override public void onStopTrackingTouch(SeekBar sb) {
                send("BRIGHT:" + sb.getProgress());
            }
        });

        setConnected(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!hasBle()) return;
        if (Build.VERSION.SDK_INT >= 31) {
            if (hasPerm(Manifest.permission.BLUETOOTH_CONNECT) && hasPerm(Manifest.permission.BLUETOOTH_SCAN)) {
                startScan();
            } else {
                ActivityCompat.requestPermissions(this,
                        new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT}, 100);
            }
        } else if (!hasPerm(Manifest.permission.ACCESS_FINE_LOCATION)) {
            ActivityCompat.requestPermissions(this,
                    new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, 100);
        } else {
            startScan();
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, @NonNull String[] perms, @NonNull int[] grants) {
        super.onRequestPermissionsResult(code, perms, grants);
        if (code == 100) {
            boolean ok = true;
            for (int g : grants) if (g != PackageManager.PERMISSION_GRANTED) ok = false;
            if (ok) startScan();
            else toast("需要蓝牙/定位权限才能扫描设备");
        }
    }

    private boolean hasBle() {
        if (btAdapter == null) { toast("本机不支持蓝牙"); return false; }
        if (!btAdapter.isEnabled()) { toast("请先打开蓝牙"); return false; }
        return true;
    }

    private boolean hasPerm(String p) {
        return ContextCompat.checkSelfPermission(this, p) == PackageManager.PERMISSION_GRANTED;
    }

    private void startScan() {
        if (!hasBle() || btScanner == null) return;
        if (scanning) return;
        scanning = true;
        foundDevices.clear();
        deviceName.setText(R.string.scanning);
        btnConnect.setEnabled(false);
        log("开始扫描 11C3 设备…");
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        if (hasPerm(Manifest.permission.BLUETOOTH_SCAN) || Build.VERSION.SDK_INT < 31) {
            btScanner.startScan(null, settings, scanCallback);
        }
        ui.postDelayed(() -> {
            if (scanning) {
                scanning = false;
                if (btScanner != null && (hasPerm(Manifest.permission.BLUETOOTH_SCAN) || Build.VERSION.SDK_INT < 31)) {
                    btScanner.stopScan(scanCallback);
                }
                if (!connected) {
                    if (foundDevices.isEmpty()) deviceName.setText(R.string.no_device);
                    else deviceName.setText(String.format(Locale.CHINA, "发现 %d 台设备，点击连接", foundDevices.size()));
                }
            }
        }, 6000);
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int type, ScanResult r) {
            BluetoothDevice d = r.getDevice();
            String name = d.getName() != null ? d.getName() : "";
            if (!name.startsWith(DEV_PREFIX)) return;
            for (BluetoothDevice f : foundDevices) if (f.getAddress().equals(d.getAddress())) return;
            foundDevices.add(d);
            deviceName.setText(name + "  (" + d.getAddress() + ")");
            btnConnect.setEnabled(true);
            log("发现设备: " + name);
        }

        @Override
        public void onScanFailed(int error) {
            scanning = false;
            deviceName.setText(R.string.no_device);
            log("扫描失败 code=" + error);
        }
    };

    private void toggleConnect() {
        if (connected) disconnect();
        else if (!foundDevices.isEmpty()) connect(foundDevices.get(0));
        else toast("请先扫描设备");
    }

    @SuppressLint("MissingPermission")
    private void connect(BluetoothDevice dev) {
        if (gatt != null) { gatt.disconnect(); gatt.close(); gatt = null; }
        targetDevice = dev;
        connStatus.setText(R.string.connecting);
        connStatus.setTextColor(ContextCompat.getColor(this, R.color.tianyi_blue));
        btnConnect.setEnabled(false);
        log("连接 " + dev.getName() + " …");
        gatt = dev.connectGatt(this, false, gattCallback);
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                if (hasPerm(Manifest.permission.BLUETOOTH_CONNECT) || Build.VERSION.SDK_INT < 31) {
                    g.discoverServices();
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                ui.post(() -> {
                    setConnected(false);
                    toast("设备已断开");
                });
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) { toast("服务发现失败"); return; }
            BluetoothGattService svc = g.getService(SVC_CTRL);
            if (svc == null) { toast("未找到 11C3 控制服务"); return; }
            BluetoothGattCharacteristic st = svc.getCharacteristic(CHR_STATUS);
            BluetoothGattCharacteristic ctrl = svc.getCharacteristic(CHR_CTRL);
            if (st == null || ctrl == null) { toast("未找到控制特征"); return; }
            boolean en = g.setCharacteristicNotification(st, true);
            if (en && Build.VERSION.SDK_INT >= 21) {
                BluetoothGattDescriptor cc = st.getDescriptor(
                        UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"));
                if (cc != null) cc.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            }
            ui.post(() -> {
                setConnected(true);
                log("已连接 " + targetDevice.getName() + "，读取状态…");
                send("STATUS");
            });
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            String text = c.getStringValue(0);
            ui.post(() -> onReply(text));
        }
    };

    private void disconnect() {
        if (gatt != null) {
            if (hasPerm(Manifest.permission.BLUETOOTH_CONNECT) || Build.VERSION.SDK_INT < 31) gatt.disconnect();
            gatt.close();
            gatt = null;
        }
        setConnected(false);
    }

    private void setConnected(boolean on) {
        connected = on;
        connStatus.setText(on ? R.string.connected : R.string.not_connected);
        connStatus.setTextColor(ContextCompat.getColor(this,
                on ? R.color.accent_green : R.color.text_secondary));
        btnConnect.setText(on ? R.string.disconnect : R.string.connect);
        btnConnect.setEnabled(on || !foundDevices.isEmpty());
        btnPlay.setEnabled(on);
        btnStop.setEnabled(on);
        btnNext.setEnabled(on);
        btnUploadPlay.setEnabled(on && !csvContent.isEmpty() && !uploading);
        seekBright.setEnabled(on);
    }

    @SuppressLint("MissingPermission")
    private void send(String cmd) {
        if (gatt == null || !connected) { toast("未连接"); return; }
        BluetoothGattService svc = gatt.getService(SVC_CTRL);
        if (svc == null) return;
        BluetoothGattCharacteristic ctrl = svc.getCharacteristic(CHR_CTRL);
        if (ctrl == null) return;
        byte[] data = cmd.getBytes(StandardCharsets.UTF_8);
        ctrl.setValue(data);
        gatt.writeCharacteristic(ctrl);
        log("→ " + cmd);
    }

    // ---------- CSV 导入与上传 ----------

    private void pickCsv() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("text/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"text/*", "application/csv"});
        try {
            startActivityForResult(intent, REQ_PICK_CSV);
        } catch (Exception e) {
            toast("无法打开文件选择器");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_PICK_CSV && resultCode == RESULT_OK && data != null && data.getData() != null) {
            csvUri = data.getData();
            csvDisplayName = queryDisplayName(csvUri);
            readCsv();
        }
    }

    private String queryDisplayName(Uri uri) {
        String name = "";
        try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) name = c.getString(idx);
            }
        }
        if (name.isEmpty()) {
            String path = uri.getLastPathSegment();
            name = path != null ? path : "show";
        }
        int dot = name.lastIndexOf('.');
        if (dot > 0) name = name.substring(0, dot);
        return name;
    }

    private void readCsv() {
        new Thread(() -> {
            try (InputStream is = getContentResolver().openInputStream(csvUri)) {
                ByteArrayOutputStream bos = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int n;
                while ((n = is.read(buf)) != -1) bos.write(buf, 0, n);
                csvContent = bos.toString("UTF-8");
                if (csvContent.length() > 100000) {
                    csvContent = "";
                    ui.post(() -> toast("CSV 过大（>100KB）"));
                    return;
                }
                int lines = csvContent.split("\n").length;
                String finalName = csvDisplayName;
                ui.post(() -> {
                    csvName.setText(finalName + "  ·  " + lines + " 行");
                    btnUploadPlay.setEnabled(connected && !uploading);
                    log("已载入 CSV: " + finalName + "（" + lines + " 行）");
                });
            } catch (Exception e) {
                ui.post(() -> toast("读取 CSV 失败: " + e.getMessage()));
            }
        }).start();
    }

    private void uploadAndPlay() {
        if (!connected) { toast("请先连接设备"); return; }
        if (csvContent.isEmpty()) { toast("请先选择 CSV"); return; }
        if (uploading) return;
        uploading = true;
        btnUploadPlay.setEnabled(false);
        btnUploadPlay.setText(R.string.uploading);
        final String name = sanitizeName(csvDisplayName);

        new Thread(() -> {
            try {
                Thread.sleep(120);
                writeCtrl("UP:" + name);
                Thread.sleep(120);
                byte[] data = csvContent.getBytes(StandardCharsets.UTF_8);
                int chunk = 180;
                int total = 0;
                for (int i = 0; i < data.length; i += chunk) {
                    int len = Math.min(chunk, data.length - i);
                    writeUpload(data, i, len);
                    total += len;
                    if (total % 1800 == 0) {
                        final int done = total;
                        ui.post(() -> log("上传中… " + done + "/" + data.length + " 字节"));
                    }
                    Thread.sleep(8);
                }
                Thread.sleep(150);
                writeCtrl("END:" + name);
                Thread.sleep(150);
                writeCtrl("LOAD:" + name);
                Thread.sleep(150);
                writeCtrl("PLAY");
                ui.post(() -> {
                    log("已上传并播放: " + name);
                    toast("已上传并播放 " + name);
                });
            } catch (Exception e) {
                ui.post(() -> toast("上传失败: " + e.getMessage()));
            } finally {
                ui.post(() -> {
                    uploading = false;
                    btnUploadPlay.setEnabled(connected && !csvContent.isEmpty());
                    btnUploadPlay.setText(R.string.upload_play);
                });
            }
        }).start();
    }

    private String sanitizeName(String raw) {
        String n = raw.replaceAll("[^\\w\\u4e00-\\u9fa5-]", "_");
        if (n.length() > 24) n = n.substring(0, 24);
        return n.isEmpty() ? "show" : n;
    }

    @SuppressLint("MissingPermission")
    private void writeCtrl(String cmd) throws Exception {
        if (gatt == null) throw new Exception("not connected");
        BluetoothGattService svc = gatt.getService(SVC_CTRL);
        BluetoothGattCharacteristic ctrl = svc != null ? svc.getCharacteristic(CHR_CTRL) : null;
        if (ctrl == null) throw new Exception("ctrl char missing");
        ctrl.setValue(cmd.getBytes(StandardCharsets.UTF_8));
        boolean ok = gatt.writeCharacteristic(ctrl);
        if (!ok) throw new Exception("write failed: " + cmd);
    }

    @SuppressLint("MissingPermission")
    private void writeUpload(byte[] data, int off, int len) throws Exception {
        if (gatt == null) throw new Exception("not connected");
        BluetoothGattService svc = gatt.getService(SVC_CTRL);
        BluetoothGattCharacteristic up = svc != null ? svc.getCharacteristic(CHR_UPLOAD) : null;
        if (up == null) throw new Exception("upload char missing");
        byte[] slice = new byte[len];
        System.arraycopy(data, off, slice, 0, len);
        up.setValue(slice);
        boolean ok = gatt.writeCharacteristic(up);
        if (!ok) throw new Exception("upload write failed");
    }

    private void sendColor() {
        StringBuilder sb = new StringBuilder("COLOR:");
        for (int i = 0; i < 10; i++) {
            Channel ch = channels[i];
            int r = (Color.red(ch.color) * 15 + 127) / 255;
            int g = (Color.green(ch.color) * 15 + 127) / 255;
            int b = (Color.blue(ch.color) * 15 + 127) / 255;
            if (i > 0) sb.append(';');
            sb.append(i).append(':').append(ch.func).append(',').append(r).append(',').append(g).append(',').append(b);
        }
        send(sb.toString());
    }

    private void onReply(String text) {
        log("← " + text);
        if (text == null) return;
        if (text.startsWith("STATUS:")) {
            String[] kv = text.split(";");
            for (String s : kv) {
                if (s.startsWith("bright=")) {
                    try {
                        int v = Integer.parseInt(s.substring(7));
                        seekBright.setProgress(v);
                        brightValue.setText(v + "%");
                    } catch (NumberFormatException ignored) {}
                }
            }
        } else if (text.startsWith("OK:")) {
            log("操作成功: " + text.substring(3));
        } else if (text.startsWith("ERR:")) {
            toast(text.substring(4));
        }
    }

    private void buildChannelRows() {
        LayoutInflater inf = getLayoutInflater();
        channelsContainer.removeAllViews();
        for (int i = 0; i < 10; i++) {
            View row = inf.inflate(R.layout.item_channel, channelsContainer, false);
            TextView label = row.findViewById(R.id.ch_label);
            View swatch = row.findViewById(R.id.ch_swatch);
            Spinner func = row.findViewById(R.id.ch_func);
            Button pick = row.findViewById(R.id.ch_pick);

            label.setText("CH" + i);
            setSwatch(swatch, channels[i].color);
            func.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, FUNCS));
            func.setSelection(0);
            final int idx = i;
            func.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                    channels[idx].func = pos;
                }
                @Override public void onNothingSelected(AdapterView<?> p) {}
            });
            pick.setOnClickListener(v -> {
                new androidx.appcompat.app.AlertDialog.Builder(this)
                        .setTitle("选择 CH" + idx + " 颜色")
                        .setItems(PALETTE_NAMES, (d, which) -> {
                            int c2 = Color.parseColor(PALETTE[which]);
                            channels[idx].color = c2;
                            setSwatch(swatch, c2);
                        })
                        .show();
            });
            channelsContainer.addView(row);
        }
    }

    private void setSwatch(View v, int color) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.RECTANGLE);
        d.setCornerRadius(6 * getResources().getDisplayMetrics().density);
        d.setColor(color);
        d.setStroke(1, Color.rgb(0xDD, 0xDD, 0xDD));
        v.setBackground(d);
    }

    private void buildPalette() {
        paletteContainer.removeAllViews();
        int density = (int) getResources().getDisplayMetrics().density;
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, 48 * density, 1);
        lp.setMargins(4, 0, 4, 0);
        for (int i = 0; i < PALETTE.length; i++) {
            String hex = PALETTE[i];
            View v = new View(this);
            GradientDrawable d = new GradientDrawable();
            d.setShape(GradientDrawable.RECTANGLE);
            d.setCornerRadius(8 * density);
            d.setColor(Color.parseColor(hex));
            d.setStroke(1, Color.rgb(0xE0, 0xE0, 0xE0));
            v.setBackground(d);
            v.setLayoutParams(lp);
            final String name = PALETTE_NAMES[i];
            v.setOnClickListener(x -> {
                for (Channel ch : channels) ch.color = Color.parseColor(hex);
                for (int j = 0; j < channelsContainer.getChildCount(); j++) {
                    View row = channelsContainer.getChildAt(j);
                    setSwatch(row.findViewById(R.id.ch_swatch), Color.parseColor(hex));
                }
                if (connected) sendColor();
                log("天依色盘「" + name + "」→ 10 通道");
            });
            paletteContainer.addView(v);
        }
    }

    private void log(String msg) {
        String cur = log.getText().toString();
        log.setText((cur.isEmpty() ? "" : cur + "\n") + msg);
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (gatt != null) {
            if (Build.VERSION.SDK_INT < 31 || hasPerm(Manifest.permission.BLUETOOTH_CONNECT)) gatt.disconnect();
            gatt.close();
        }
    }
}
