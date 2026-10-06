/* 
 *	Copyright (C) 2009 cooleyes
 *	eyes.cooleyes@gmail.com 
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */
 
static char* i18n_msg[] = {
	
#ifdef ENG
	"OK",
	"Cancel",
	"Yes",
	"NO",
	"On",
	"Off",
	"<Not used>",
	"CPU Speed: ",
	"Subtitles Charset: ",
	"Subtitles FontSize: ",
	"Subtitles Embolden: ",
	"Subtitles Position Align: ",
	"TOP",
	"BOTTOM",
	"Subtitles Position Distance: ",
	"FileSystem Charset: ",
	"", "", "", "", "", "", /* Reserved ABI slots. */
	"Play Mode: ",
	"SINGLE",
	"GROUP",
	"ALL",
	"TV AspectRatio: ",
	"TV OverScan(L.T.R.B): ",
	"Video Mode: ",
	"PSP LCD",
	"Composite",
	"Component Interlace",
	"Component Progressive",
	"Configuration",
	"Skins",
	"Are you sure you want to delete this ?",
	"Do you want to quit the PPA ?"
#endif

#ifdef CHS
	"确认",
	"取消",
	"是",
	"否",
	"开启",
	"关闭",
	"<未使用>",
	"CPU速度: ",
	"字幕编码: ",
	"字幕字体大小: ",
	"字幕粗体显示: ",
	"字幕显示位置: ",
	"顶部",
	"底部",
	"字幕边界距离: ",
	"文件系统编码: ",
	"", "", "", "", "", "", /* Reserved ABI slots. */
	"播放模式: ",
	"单个",
	"组播",
	"全部",
	"电视比例: ",
	"电视过扫描: ",
	"输出模式: ",
	"本机屏幕",
	"S端子",
	"隔行色差",
	"逐行色差",
	"设置",
	"皮肤",
	"您确定要删除选定的文件吗?",
	"您是否希望退出播放器?"
#endif

#ifdef CHT
	"確認",
	"取消",
	"是",
	"否",
	"開啟",
	"關閉",
	"<未使用>",
	"CPU時脈: ",
	"字幕編碼: ",
	"字幕字體大小: ",
	"字幕粗體顯示: ",
	"字幕顯示位置: ",
	"頂部",
	"底部",
	"字幕邊界距離: ",
	"文件系統編碼: ",
	"", "", "", "", "", "", /* Reserved ABI slots. */
	"播放模式: ",
	"單個",
	"組播",
	"全部",
	"電視比例: ",
	"電視過掃描: ",
	"顯示模式: ",
	"本機螢幕",
	"S端子",
	"隔行色差",
	"逐行色差",
	"設定",
	"皮肤",
	"您確定要刪除選取的文件嗎?",
	"您是否希望離開播放器?"
#endif

};

char* i18n_get_text(int entry) {
	return i18n_msg[entry];
}