CoA Patcher
===========
A small window for applying mod-coa-playerbots .patch files and rebuilding the bot worldserver.
It is one PowerShell script (CoAPatcher.ps1) using only what ships with Windows 10.
Nothing is installed. You can open the script in Notepad and read every line.

Start it
--------
1. Unzip this folder anywhere, for example F:\CoA\CoAPatcher.
2. Double-click "Run CoAPatcher.bat".
   If Windows shows "Windows protected your PC" or a security warning, choose More info > Run anyway
   (or right-click the .ps1 > Properties > Unblock).
3. Check the paths at the top. They start with your usual folders. They are saved to
   CoAPatcher.settings.json next to the script. That file holds the MySQL password (root) in plain text.

Use it
------
- Drag .patch files into the list, or use Browse. Each one is copied into your patch dir (F:\CoA\patches).
  Patches go on top to bottom. Use Move up / Move down / Remove to change the order.
  Remove only takes a patch off the list. The file stays in the patch dir.
- Check only: a dry run on a temporary copy of the module. Your files are not touched.
- Apply & Build does all of this in order and stops at the first problem:
    1. Backs up the ticked databases with mysqldump to the backup dir as <db>_<yyyyMMdd_HHmmss>.sql.
    2. Checks every patch on a temporary copy. If any patch fails, nothing is applied.
       A patch that is already in shows "already applied, skipping".
    3. Applies the patches to modules\mod-coa-playerbots with
       git apply -p2 --directory=modules/mod-coa-playerbots --exclude=*.md
       If anything goes wrong partway, it reverses the patches it applied in that run.
    4. Asks before stopping the bot worldserver (only the one running from your runtime folder;
       it never touches authserver or your old server), then runs
       cmake --build <build dir> --config RelWithDebInfo --target worldserver
    5. Starts worldserver.exe again if "Start worldserver after build" is ticked.
       worldserver applies any new SQL updates itself when it starts.
- Build only: stop worldserver (after asking), build, and optionally start it again.
- Backup only: just the mysqldump backups.
- Undo last patch: reverses the most recent patch this app applied. Press Build only afterwards.
- Every run writes a log to the backup dir (CoAPatcher_<time>_<action>.log).

Addons are not part of this app. Install them yourself as usual.

What it never does
------------------
git reset / clean / checkout, dropping or importing databases, touching authserver, opening network ports.
mysqldump connects to 127.0.0.1 and gets the password from a temporary option file that is deleted afterwards.

It finds git on PATH, then in GitHub Desktop's bundled copy, then in Program Files\Git.
It finds cmake on PATH, then in the copy bundled with Visual Studio (found with vswhere).
