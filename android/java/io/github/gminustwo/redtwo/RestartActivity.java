// Restarts Red Two: runs in a process of its own (AndroidManifest.xml), ends the game's process
// (whose card and save state writes are done) and starts the game's activity again.
package io.github.gminustwo.redtwo;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

public class RestartActivity extends Activity {
    static final String EXTRA_PID = "pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0) Process.killProcess(pid);
        Intent i = new Intent(this, RedTwoActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
