package com.pigeongames.timing;

import android.app.Activity;
import android.app.Application;
import android.app.AlertDialog;
import android.widget.*;
import android.view.Gravity;
import org.json.JSONArray;
import org.json.JSONObject;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;
import android.view.ViewGroup;
import android.view.MotionEvent;
import android.util.Log;
import java.util.WeakHashMap;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Own-window drawing only: no overlay permission, input listener or gesture handling. */
public final class TimingBridge extends ContentProvider {
    static boolean loaded;
    static float practiceRate=1;
    static native boolean setPracticeSpeed(float rate);
    static boolean showTimingBar=true,showNoteTimings=true;
    static {
        try { System.loadLibrary("phigrostiming"); loaded = true; }
        catch (Throwable error) { Log.e("PhigrosTiming", "Timing library unavailable", error); }
    }
    static native float[] snapshot();
    static native float[] practiceData();
    static native int recordRevision();
    static native void setDisplayOptions(boolean bar,boolean notes);
    static final java.util.concurrent.ExecutorService recordWriter=java.util.concurrent.Executors.newSingleThreadExecutor(r->{Thread t=new Thread(r,"PracticeRecordWriter");t.setDaemon(true);return t;});
    static final java.util.concurrent.atomic.AtomicBoolean retryRecordSave=new java.util.concurrent.atomic.AtomicBoolean();
    static native String restartTarget();
    static native boolean practiceFrom(float start);
    static void seekPractice(float time) {
        String target=restartTarget();
        practiceAction(1,time);
        retryResult(target);
    }
    static void retryResult(String target) {
        if(!target.isEmpty()) try {
            Class.forName("com.unity3d.player.UnityPlayer").getMethod("UnitySendMessage",String.class,String.class,String.class).invoke(null,target,"Retry","");
        } catch(Exception error) {Log.e("PhigrosTiming","Practice restart failed",error);}
    }
    static native void practiceAction(int command,float value);
    static native void configureUnlock(String auditPath);
    final WeakHashMap<Activity, View> views = new WeakHashMap<>();

    @Override public boolean onCreate() {
        Application app = (Application)getContext().getApplicationContext();
        android.content.SharedPreferences displayPrefs=app.getSharedPreferences("practice-controls",0);
        showTimingBar=displayPrefs.getBoolean("show-timing-bar",true);
        showNoteTimings=displayPrefs.getBoolean("show-note-timings",true);
        if (loaded) {
            setDisplayOptions(showTimingBar,showNoteTimings);
            try {
                // Before Unity starts, preserve this independent app's data.
                // Failed backup prevents any unlock/record migration.
                File directory = new File(app.getExternalFilesDir(null), "timing-backups");
                if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Backup directory unavailable");
                for (String backupName : new String[]{"before-unlock-v1.zip", "before-all-songs-v1.zip", "before-practice-v1.zip"}) {
                File backup = new File(directory, backupName);
                if (!backup.isFile()) {
                    File temporary = new File(directory, "before-unlock-v1.tmp");
                    try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(temporary))) {
                        File data = new File(app.getApplicationInfo().dataDir);
                        for (String name : new String[]{"shared_prefs", "files", "databases", "no_backup"})
                            backupTree(new File(data, name), "internal/"+name, zip);
                    }
                    if (!temporary.renameTo(backup)) throw new IOException("Backup rename failed");
                }
                }
                configureUnlock(new File(directory, "unlock-audit.json").getAbsolutePath());
            } catch (Throwable error) { Log.e("PhigrosTiming", "Unlock disabled: backup failed", error); }
        }
        app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            public void onActivityCreated(Activity activity, Bundle state) {}
            public void onActivityStarted(Activity activity) {}
            public void onActivityResumed(Activity activity) {
                if (!loaded || views.containsKey(activity)) return;
                // Limit display to the Unity gameplay host, not login/payment activities.
                String name = activity.getClass().getName();
                if (!name.contains("UnityPlayer") && !name.equals("com.PigeonGames.Phigros.MainActivity")) return;
                ViewGroup root = (ViewGroup)activity.getWindow().getDecorView();
                FrameLayout hud = new FrameLayout(activity);
                hud.addView(new TimingView(activity),new FrameLayout.LayoutParams(-1,-1));
                addPracticeControls(activity,hud);
                root.addView(hud, new ViewGroup.LayoutParams(-1, -1));
                views.put(activity, hud);
                Log.i("PhigrosTiming", "Non-touchable HUD attached to " + name);
            }
            public void onActivityPaused(Activity activity) {}
            public void onActivityStopped(Activity activity) {}
            public void onActivitySaveInstanceState(Activity activity, Bundle state) {}
            public void onActivityDestroyed(Activity activity) {
                View hud = views.remove(activity);
                if (hud != null && hud.getParent() instanceof ViewGroup)
                    ((ViewGroup)hud.getParent()).removeView(hud);
            }
        });
        return true;
    }

    static void addPracticeControls(Activity activity,FrameLayout root) {
        Button menu=new Button(activity);menu.setText("练习");menu.setTextSize(11);
        float density=activity.getResources().getDisplayMetrics().density;
        int size=(int)(48*density),edge=(int)(12*density),gap=(int)(8*density);
        // Separate bottom-corner targets; the empty space between them remains non-interactive.
        FrameLayout.LayoutParams mp=new FrameLayout.LayoutParams(size,size,Gravity.LEFT|Gravity.BOTTOM);
        mp.leftMargin=edge;mp.bottomMargin=edge;
        root.addView(menu,mp);
        Button back=new Button(activity),forward=new Button(activity),restart=new Button(activity);
        restart.setText("重练");restart.setTextSize(11);
        final android.content.SharedPreferences prefs=activity.getSharedPreferences("practice-controls",0);
        final float[] steps={prefs.getFloat("back",4),prefs.getFloat("forward",4)};
        Runnable labels=()->{back.setText("≪ "+steps[0]+"s");forward.setText(steps[1]+"s ≫");};labels.run();back.setTextSize(11);forward.setTextSize(11);
        FrameLayout.LayoutParams bp=new FrameLayout.LayoutParams(size,size,Gravity.LEFT|Gravity.BOTTOM);bp.leftMargin=edge+size+gap;bp.bottomMargin=edge;
        FrameLayout.LayoutParams fp=new FrameLayout.LayoutParams(size,size,Gravity.RIGHT|Gravity.BOTTOM);fp.rightMargin=edge+size+gap;fp.bottomMargin=edge;
        root.addView(back,bp);root.addView(forward,fp);
        FrameLayout.LayoutParams rp=new FrameLayout.LayoutParams(size,size,Gravity.RIGHT|Gravity.BOTTOM);
        rp.rightMargin=edge;rp.bottomMargin=edge;
        root.addView(restart,rp);restart.setVisibility(View.GONE);
        restart.setOnClickListener(v->practiceAction(3,0));
        final boolean[] armed={false};
        back.setVisibility(View.GONE);forward.setVisibility(View.GONE);
        back.setOnClickListener(v->{float[] d=practiceData();seekPractice(Math.max(0,d[0]-steps[0]));});
        forward.setOnClickListener(v->{float[] d=practiceData();seekPractice(d[0]+steps[1]);});
        menu.setOnClickListener(v->{
            float[] data=practiceData();
            LinearLayout body=new LinearLayout(activity);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(24,12,24,12);
            TextView explanation=new TextView(activity);explanation.setText("先勾选记录，再关闭此窗口并正常开始歌曲。打歌中查看失误前，请先点游戏左上角暂停。片段练习不保存成绩和 RKS。");body.addView(explanation);
            Button speed=new Button(activity);speed.setText(String.format(java.util.Locale.ROOT,"变速练习：%.2f 倍（音调随速变化）",practiceRate));
            body.addView(speed);
            CheckBox barToggle=new CheckBox(activity);barToggle.setText("显示准度条提示");barToggle.setChecked(showTimingBar);body.addView(barToggle);
            barToggle.setOnCheckedChangeListener((b,on)->{showTimingBar=on;setDisplayOptions(showTimingBar,showNoteTimings);prefs.edit().putBoolean("show-timing-bar",on).apply();});
            CheckBox noteToggle=new CheckBox(activity);noteToggle.setText("显示每个音符的准度（早／晚及毫秒数）");noteToggle.setChecked(showNoteTimings);body.addView(noteToggle);
            noteToggle.setOnCheckedChangeListener((b,on)->{showNoteTimings=on;setDisplayOptions(showTimingBar,showNoteTimings);prefs.edit().putBoolean("show-note-timings",on).apply();});
            CheckBox capture=new CheckBox(activity);capture.setText("记录本次游玩（排除 Perfect）");capture.setChecked(data[3]!=0);body.addView(capture);
            capture.setOnCheckedChangeListener((b,on)->practiceAction(0,on?1:0));
            CheckBox enable=new CheckBox(activity);enable.setText("启用快进、后退和重练按钮");enable.setChecked(armed[0]);body.addView(enable);
            enable.setOnCheckedChangeListener((b,on)->{armed[0]=on;back.setVisibility(on?View.VISIBLE:View.GONE);forward.setVisibility(on?View.VISIBLE:View.GONE);restart.setVisibility(on?View.VISIBLE:View.GONE);});
            ListView list=new ListView(activity){
                @Override public boolean dispatchTouchEvent(MotionEvent event){
                    getParent().requestDisallowInterceptTouchEvent(event.getActionMasked()!=MotionEvent.ACTION_UP && event.getActionMasked()!=MotionEvent.ACTION_CANCEL);
                    return super.dispatchTouchEvent(event);
                }
            };body.addView(list,new LinearLayout.LayoutParams(-1,size*5));
            ScrollView menuScroll=new ScrollView(activity);menuScroll.addView(body);
            AlertDialog dialog=new AlertDialog.Builder(activity).setTitle("失误记录与片段练习").setView(menuScroll).setNegativeButton("关闭",null).create();
            final int count=(data.length-4)/4;
            TextView empty=new TextView(activity);empty.setText("勾选记录后开始打歌，Good / Bad / Miss 将显示在这里。");body.addView(empty);list.setEmptyView(empty);
            list.setAdapter(new BaseAdapter(){
                public int getCount(){return count;}
                public Object getItem(int position){return data[4+position*4];}
                public long getItemId(int position){return position;}
                public View getView(int position,View recycled,ViewGroup parent){
                    Button row=recycled instanceof Button?(Button)recycled:new Button(activity);
                    final float time=data[4+position*4];int grade=(int)data[5+position*4];float ms=data[6+position*4];
                    String offset=grade==3?"":String.format(java.util.Locale.ROOT,"  %s %.1f ms",ms<0?"早":"晚",Math.abs(ms));
                    row.setText(String.format(java.util.Locale.ROOT,"%02d:%05.2f  %s%s",(int)time/60,time%60,grade==1?"Good":grade==2?"Bad":"Miss",offset));
                    row.setOnClickListener(w->{seekPractice(Math.max(0,time-2));dialog.dismiss();});return row;
                }
            });
            Button here=new Button(activity);here.setText("从当前时间开始练习（不限时）");here.setOnClickListener(w->{seekPractice(Math.max(0,data[0]));dialog.dismiss();});body.addView(here);
            Button settings=new Button(activity);settings.setText("设置练习起点 / 前后跳转秒数");
            settings.setOnClickListener(w->{dialog.dismiss();showRangeSettings(activity,steps,labels,prefs);});
            body.addView(settings,body.indexOfChild(enable)+1);
            speed.setOnClickListener(w->{dialog.dismiss();dialogForSpeed(activity);});
            dialog.show();
        });
        root.post(new Runnable(){int savedRevision=-1;public void run(){
            if(activity.isDestroyed())return;
            try {
                int revision=recordRevision();
                if(revision!=savedRevision || retryRecordSave.getAndSet(false)) {
                    float[] data=practiceData();
                    if(data[3]!=0 || data.length>4) {
                        File target=new File(activity.getFilesDir(),"practice-latest.json");
                        recordWriter.execute(()->savePracticeRecord(target,data));
                    }
                    savedRevision=revision;
                }
            }catch(Throwable error){Log.e("PhigrosTiming","Practice record update failed",error);}
            root.postDelayed(this,1000);
        }});
    }

    static void savePracticeRecord(File target,float[] data) {
        android.util.AtomicFile file=new android.util.AtomicFile(target);FileOutputStream out=null;
        try {
            JSONArray rows=new JSONArray();
            for(int i=4;i+3<data.length;i+=4){JSONArray row=new JSONArray();for(int j=0;j<4;j++)row.put(data[i+j]);rows.put(row);}
            JSONObject record=new JSONObject();record.put("format",1);record.put("fields",new JSONArray(new String[]{"chartTimeSeconds","gradeGood1Bad2Miss3","deltaMilliseconds","noteCode"}));record.put("mistakes",rows);
            out=file.startWrite();out.write(record.toString().getBytes("UTF-8"));file.finishWrite(out);
        }catch(Throwable error){if(out!=null)file.failWrite(out);retryRecordSave.set(true);Log.e("PhigrosTiming","Practice record save failed",error);}
    }


    static void dialogForSpeed(Activity activity) {
        LinearLayout body=new LinearLayout(activity);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(24,12,24,12);
        TextView help=new TextView(activity);help.setText("范围 0.50～1.50 倍。音乐音调随速度变化，音符和判定线同步变速；谱面内容不变。\n倍率对当前及后续歌曲生效，重启应用恢复 1 倍。变速后的本次游玩属于练习，不保存成绩或 RKS，恢复原速后也一样。\n起点、跳转秒数和准度毫秒沿用原谱时间，慢速判定窗口随之放宽。");body.addView(help);
        EditText rate=secondsInput(activity,body,"播放倍率",practiceRate);
        LinearLayout presets=new LinearLayout(activity);body.addView(presets);
        for(float value:new float[]{.5f,.75f,1f,1.25f}) {
            Button button=new Button(activity);button.setText(Float.toString(value)+"×");
            button.setOnClickListener(v->rate.setText(Float.toString(value)));
            presets.addView(button,new LinearLayout.LayoutParams(0,-2,1));
        }
        ScrollView scroll=new ScrollView(activity);scroll.addView(body);
        AlertDialog dialog=new AlertDialog.Builder(activity).setTitle("变速练习").setView(scroll)
            .setPositiveButton("应用倍率",null).setNeutralButton("恢复 1 倍",(d,w)->{setPracticeSpeed(1);practiceRate=1;Toast.makeText(activity,"已恢复 1 倍；本次变速练习仍不保存成绩",Toast.LENGTH_LONG).show();}).setNegativeButton("取消",null).create();
        dialog.setOnShowListener(v->dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(w->{
            try {
                float value=seconds(rate);
                if(!setPracticeSpeed(value)){rate.setError("请填写 0.50～1.50 之间的倍率");return;}
                practiceRate=value;dialog.dismiss();
                Toast.makeText(activity,String.format(java.util.Locale.ROOT,"练习倍率 %.2f×；谱面数据保持原样",value),Toast.LENGTH_LONG).show();
            }catch(IllegalArgumentException error){rate.setError("请填写有效倍率");}
        }));
        dialog.show();
    }

    static EditText secondsInput(Activity activity,LinearLayout body,String label,float value) {
        TextView title=new TextView(activity);title.setText(label);body.addView(title);
        EditText input=new EditText(activity);input.setSingleLine(true);
        input.setInputType(android.text.InputType.TYPE_CLASS_NUMBER|android.text.InputType.TYPE_NUMBER_FLAG_DECIMAL);
        input.setText(Float.toString(value));body.addView(input);return input;
    }
    static float seconds(EditText input) {
        float value=Float.parseFloat(input.getText().toString().trim());
        if(Float.isNaN(value)||Float.isInfinite(value))throw new IllegalArgumentException();
        return value;
    }
    static void showRangeSettings(Activity activity,float[] steps,Runnable labels,android.content.SharedPreferences prefs) {
        LinearLayout body=new LinearLayout(activity);body.setOrientation(LinearLayout.VERTICAL);body.setPadding(24,12,24,12);
        TextView help=new TextView(activity);help.setText("单位为秒，可填小数，例如 65.5。只设置起点，持续练到曲末；快进、后退会更新起点，重练立即返回起点。跨过的长条头会跳过，不再拉回。无预备倒计时。");body.addView(help);
        float now=Math.max(0,practiceData()[0]);
        EditText start=secondsInput(activity,body,"开始时间（秒）",now);
        EditText back=secondsInput(activity,body,"回退秒数",steps[0]);
        EditText forward=secondsInput(activity,body,"快进秒数",steps[1]);
        ScrollView scroll=new ScrollView(activity);scroll.addView(body);
        AlertDialog dialog=new AlertDialog.Builder(activity).setTitle("练习起点与跳转设置").setView(scroll)
            .setPositiveButton("开始练习",null).setNeutralButton("仅保存跳转秒数",null).setNegativeButton("取消",null).create();
        dialog.setOnShowListener(v->{
            View.OnClickListener apply=w->{
                try {
                    float b=seconds(back),f=seconds(forward);
                    if(b<=0||f<=0)throw new IllegalArgumentException();
                    if(w==dialog.getButton(AlertDialog.BUTTON_POSITIVE)) {
                        float a=seconds(start);
                        if(a<0) {start.setError("开始时间不能为负数");return;}
                        String target=restartTarget();
                        if(!practiceFrom(a)){start.setError("开始时间超出曲长或范围无效");return;}
                        retryResult(target);
                    }
                    steps[0]=b;steps[1]=f;prefs.edit().putFloat("back",b).putFloat("forward",f).apply();labels.run();dialog.dismiss();
                } catch(IllegalArgumentException error) {Toast.makeText(activity,"请填写有效秒数，回退和快进必须大于 0",Toast.LENGTH_LONG).show();}
            };
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(apply);
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(apply);
        });
        dialog.show();
    }

    static void backupTree(File file, String name, ZipOutputStream zip) throws IOException {
        if (!file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("Cannot read backup directory");
            for (File child : children) backupTree(child, name+"/"+child.getName(), zip);
        } else {
            zip.putNextEntry(new ZipEntry(name));
            try (FileInputStream input = new FileInputStream(file)) {
                byte[] buffer = new byte[32768];
                int length;
                while ((length=input.read(buffer))!=-1) zip.write(buffer, 0, length);
            }
            zip.closeEntry();
        }
    }
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) { return null; }
    public String getType(Uri uri) { return null; }
    public Uri insert(Uri uri, ContentValues values) { return null; }
    public int delete(Uri uri, String selection, String[] args) { return 0; }
    public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }

    static final class TimingView extends View {
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        final Path arrow = new Path();
        final RectF[] spotLabels = new RectF[32];
        final int[] colors = {0xffef3340,0xffffa600,0xfff2e939,0xff55e545,0xfff2e939,0xffffa600,0xffef3340};
        final float[] edges = {-220,-160,-80,-40,40,80,160,220};
        TimingView(Activity activity) {
            super(activity);
            setClickable(false); setLongClickable(false); setFocusable(false);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
            setWillNotDraw(false);
            paint.setTextAlign(Paint.Align.CENTER);
            for (int i=0;i<spotLabels.length;i++) spotLabels[i]=new RectF();
        }
        @Override public boolean onTouchEvent(MotionEvent event) { return false; }
        float map(float ms, float left, float width) {
            return left + (Math.max(-220, Math.min(220, ms)) + 220) / 440 * width;
        }
        int bucket(float ms) {
            int i = 0;
            while (i < 6 && ms > edges[i + 1]) ++i;
            return i;
        }
        String timingLabel(float ms) {
            long tenths = Math.round(Math.abs(ms)*10);
            return (ms < 0 ? "早 " : ms > 0 ? "晚 " : "正好 ")
                + (tenths/10) + "." + (tenths%10) + " ms";
        }
        void drawNoteTimings(Canvas canvas, float[] data, float unit) {
            int count = Math.min(32, Math.max(0, (int)data[37]));
            float textSize = unit*.012f, padding = unit*.004f;
            float row = textSize + padding*2, radius = unit*.004f;
            int placed = 0;
            // Newer labels get first choice of position. Keep a line back to the
            // exact hit anchor even when nearby chord labels need separating.
            for (int i=count-1;i>=0;i--) {
                int at=38+i*4;
                float ms=data[at], x=data[at+1]*getWidth(), y=data[at+2]*getHeight(), age=data[at+3];
                if (!Float.isFinite(ms) || !Float.isFinite(x) || !Float.isFinite(y) || age<0 || age>=1000) continue;
                int alpha=(int)(255*Math.min(1f,(1000-age)/300f));
                int color=ms<0 ? 0xff67d6ff : ms>0 ? 0xffffbc68 : 0xff8cffb0;
                String label=timingLabel(ms);
                paint.setTextSize(textSize);
                float half=paint.measureText(label)/2+padding;
                float cx=Math.max(half+padding,Math.min(getWidth()-half-padding,x));
                RectF box=spotLabels[placed];
                float topLimit=getHeight()*.09f, bottomLimit=getHeight()*.94f-row;
                for(int candidate=0;candidate<12;candidate++) {
                    float offset=(candidate/2+1)*row*1.15f;
                    float top=(candidate%2==0) ? y-offset : y+offset-row;
                    top=Math.max(topLimit,Math.min(bottomLimit,top));
                    box.set(cx-half,top,cx+half,top+row);
                    boolean overlap=false;
                    for(int j=0;j<placed;j++) if(RectF.intersects(box,spotLabels[j])) { overlap=true; break; }
                    if(!overlap) break;
                }
                paint.setColor((color&0xffffff)|(alpha<<24));
                paint.setStrokeWidth(unit*.001f); paint.setStyle(Paint.Style.STROKE);
                canvas.drawCircle(x,y,radius,paint);
                canvas.drawLine(x,y,cx,Math.max(box.top,Math.min(box.bottom,y)),paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(((alpha*3/4)<<24));
                canvas.drawRoundRect(box,padding,padding,paint);
                paint.setColor((color&0xffffff)|(alpha<<24));
                canvas.drawText(label,cx,box.top+padding+textSize*.82f,paint);
                placed++;
            }
        }
        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            postInvalidateDelayed(33);
            if(!showTimingBar && !showNoteTimings) return;
            float[] data;
            try { data = snapshot(); }
            catch (Throwable error) { return; }
            if (data == null || data.length != 166 || data[1] == 0) return;
            float unit = Math.min(getWidth(), getHeight() * 16f / 9f);
            if(showNoteTimings) drawNoteTimings(canvas,data,unit);
            if(!showTimingBar) return;
            // The game's score/combo HUD follows the full screen, including 4:3 tablets.
            // Anchor in their top gap, rather than the centered 16:9 chart viewport.
            float left = getWidth() * .575f, width = getWidth() * .21f;
            float top = getHeight() * .018f;
            float bar = top + unit * .018f;
            paint.setColor(0x26000000);
            canvas.drawRoundRect(left-unit*.004f, top-unit*.004f, left+width+unit*.004f, top+unit*.049f,
                unit*.003f,unit*.003f,paint);
            paint.setColor(Color.WHITE); paint.setTextSize(unit*.009f);
            String label = data[2] == 0 ? "等待击打" : timingLabel(data[5]);
            if(practiceRate!=1) label=String.format(java.util.Locale.ROOT,"%.2f×  %s（谱面）",practiceRate,label);
            canvas.drawText(label,left+width/2,top+unit*.007f,paint);
            for (int i=0;i<7;i++) {
                float a=map(edges[i],left,width), b=map(edges[i+1],left,width);
                paint.setColor(colors[i]);
                canvas.drawRect(a,bar,b,bar+unit*.004f,paint);
                paint.setTextSize(unit*.0075f);
                canvas.drawText(Integer.toString((int)data[6+i]),(a+b)/2,bar+unit*.017f,paint);
            }
            int count = Math.min(24,(int)data[2]);
            for(int i=0;i<count;i++) {
                float ms=data[13+i], x=map(ms,left,width);
                int alpha=(int)(64+166f*(i+1)/count);
                paint.setColor((colors[bucket(ms)]&0xffffff)|(alpha<<24));
                canvas.drawRect(x-unit*.00055f,bar-unit*.0065f,x+unit*.00055f,bar-unit*.0015f,paint);
            }
            paint.setColor(Color.WHITE);
            float zero=left+width/2;
            canvas.drawRect(zero-unit*.00035f,bar-unit*.002f,zero+unit*.00035f,bar+unit*.006f,paint);
            if(count>0) {
                float x=map(data[5],left,width);
                canvas.drawRect(x-unit*.00065f,bar-unit*.0015f,x+unit*.00065f,bar+unit*.006f,paint);
                arrow.reset();arrow.moveTo(x,bar+unit*.0065f);
                arrow.lineTo(x-unit*.0028f,bar+unit*.010f);
                arrow.lineTo(x+unit*.0028f,bar+unit*.010f);arrow.close();canvas.drawPath(arrow,paint);
            }
            paint.setTextSize(unit*.0075f);
            canvas.drawText("HITS "+(int)data[3]+"   MISS "+(int)data[4],left+width/2,top+unit*.043f,paint);
        }
    }
}
