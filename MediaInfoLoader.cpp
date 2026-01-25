//----------------------------------------------------------------------------------
//	get_media_info デバッグ用 汎用プラグイン for AviUtl ExEdit2
//----------------------------------------------------------------------------------
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <vector>

#include "plugin2.h"
#include "logger2.h"

static LOG_HANDLE* g_logger = nullptr;
static EDIT_HANDLE* g_edit = nullptr;
static HWND g_dialog = nullptr;

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define IDC_MEDIA_LABEL 2001
#define IDC_MEDIA_PATH 2002
#define IDC_MEDIA_BROWSE 2003
#define IDC_MEDIA_STATUS 2004
#define IDC_MEDIA_OK 2005
#define IDC_MEDIA_CANCEL 2006

static void LogInfo(LPCWSTR message) {
	if (g_logger && g_logger->info) {
		g_logger->info(g_logger, message);
	}
}

static void LogWarn(LPCWSTR message) {
	if (g_logger && g_logger->warn) {
		g_logger->warn(g_logger, message);
	}
}

static std::wstring OpenMediaFileDialog(HWND owner) {
	wchar_t file_buffer[4096] = L"";

	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner;
	ofn.lpstrFile = file_buffer;
	ofn.nMaxFile = static_cast<DWORD>(sizeof(file_buffer) / sizeof(file_buffer[0]));
	ofn.lpstrFilter =
		L"Media Files\0*.mp4;*.mov;*.avi;*.mkv;*.webm;*.mp3;*.wav;*.flac;*.aac;*.m4a;*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
		L"All Files\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

	if (GetOpenFileNameW(&ofn)) {
		return file_buffer;
	}
	return L"";
}

static const wchar_t* GetFileNamePart(const std::wstring& path) {
	const size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) {
		return path.c_str();
	}
	return path.c_str() + pos + 1;
}

class DialogBuilder {
	std::vector<unsigned char> data;
public:
	void Align() { while (data.size() % 4 != 0) data.push_back(0); }
	void Add(const void* p, size_t s) { const unsigned char* b = (const unsigned char*)p; data.insert(data.end(), b, b + s); }
	void AddW(WORD w) { Add(&w, 2); }
	void AddD(DWORD d) { Add(&d, 4); }
	void AddS(LPCWSTR s) { if (!s) AddW(0); else Add(s, (wcslen(s) + 1) * 2); }

	void Begin(LPCWSTR title, int w, int h, DWORD style) {
		data.clear();
		Align();
		DWORD exStyle = WS_EX_CONTROLPARENT;
		AddD(style); AddD(exStyle);
		AddW(0);
		AddW(0); AddW(0); AddW(w); AddW(h);
		AddW(0); AddW(0); AddS(title); AddW(9); AddS(L"Yu Gothic UI");
	}

	void AddControl(LPCWSTR className, LPCWSTR text, WORD id, int x, int y, int w, int h, DWORD style) {
		Align();
		AddD(style | WS_CHILD | WS_VISIBLE);
		AddD(0); AddW((WORD)x); AddW((WORD)y); AddW((WORD)w); AddW((WORD)h); AddW(id);

		if (wcscmp(className, L"BUTTON") == 0) {
			AddW(0xFFFF); AddW(0x0080);
			AddS(text); AddW(0);
		} else if (wcscmp(className, L"EDIT") == 0) {
			AddW(0xFFFF); AddW(0x0081);
			AddS(text); AddW(0);
		} else if (wcscmp(className, L"STATIC") == 0) {
			AddW(0xFFFF); AddW(0x0082);
			AddS(text); AddW(0);
		} else {
			AddS(className);
			AddW(0);
			AddS(text);
			AddW(0);
		}

		if (data.size() >= 10) {
			WORD* pCount = (WORD*)&data[8];
			(*pCount)++;
		}
	}
	DLGTEMPLATE* Get() { return (DLGTEMPLATE*)data.data(); }
};

struct MediaDialogState {
	std::wstring file_path;
	std::wstring status;
};

static void ApplyImportMedia(void* param, EDIT_SECTION* edit) {
	auto* state = reinterpret_cast<MediaDialogState*>(param);
	if (!state || !edit || !edit->info) {
		if (state) state->status = L"EDIT_SECTION が無効です";
		return;
	}

	if (!edit->is_support_media_file(state->file_path.c_str(), true)) {
		state->status = L"対応していないメディアファイルです";
		return;
	}

	MEDIA_INFO info{};
	if (!edit->get_media_info(state->file_path.c_str(), &info, sizeof(info))) {
		state->status = L"get_media_info に失敗しました";
	} else {
		wchar_t info_text[256]{};
		swprintf_s(info_text, L"media info: video=%d audio=%d time=%.3f size=%dx%d",
				   info.video_track_num,
				   info.audio_track_num,
				   info.total_time,
				   info.width,
				   info.height);
		state->status = info_text;
	}

	const int layer = edit->info->layer;
	const int frame = edit->info->frame;
	// ミリ秒単位で処理を待機（デバッグ用）
	//Sleep(200); // 200ms
	//LogInfo(L"待機後にオブジェクトを作成します (200ms)");
	OBJECT_HANDLE obj = edit->create_object_from_media_file(state->file_path.c_str(), layer, frame, 0);
	if (!obj) {
		state->status += L" / create_object_from_media_file 失敗";
		return;
	}

	edit->set_object_name(obj, GetFileNamePart(state->file_path));
	state->status += L" / 作成完了";
}

static INT_PTR CALLBACK MediaDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_INITDIALOG: {
		auto* state = reinterpret_cast<MediaDialogState*>(lp);
		SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)state);
		g_dialog = hwnd;
		SetDlgItemTextW(hwnd, IDC_MEDIA_STATUS, L"メディアを選択してください");
		return TRUE;
	}
	case WM_COMMAND: {
		switch (LOWORD(wp)) {
		case IDC_MEDIA_BROWSE: {
			auto* state = reinterpret_cast<MediaDialogState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
			if (!state) return TRUE;
			std::wstring path = OpenMediaFileDialog(hwnd);
			if (!path.empty()) {
				state->file_path = path;
				SetDlgItemTextW(hwnd, IDC_MEDIA_PATH, path.c_str());
				SetDlgItemTextW(hwnd, IDC_MEDIA_STATUS, L"読み込み準備完了");
			}
			return TRUE;
		}
		case IDC_MEDIA_OK: {
			auto* state = reinterpret_cast<MediaDialogState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
			if (!state || state->file_path.empty()) {
				SetDlgItemTextW(hwnd, IDC_MEDIA_STATUS, L"ファイルを選択してください");
				return TRUE;
			}
			if (!g_edit) {
				SetDlgItemTextW(hwnd, IDC_MEDIA_STATUS, L"編集ハンドルが無効です");
				return TRUE;
			}
			state->status.clear();
			g_edit->call_edit_section_param(state, ApplyImportMedia);
			SetDlgItemTextW(hwnd, IDC_MEDIA_STATUS, state->status.c_str());
			if (!state->status.empty()) {
				LogInfo(state->status.c_str());
			}
			return TRUE;
		}
		case IDC_MEDIA_CANCEL:
			DestroyWindow(hwnd);
			return TRUE;
		}
		break;
	}
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return TRUE;
	case WM_NCDESTROY: {
		auto* state = reinterpret_cast<MediaDialogState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
		if (state) delete state;
		if (g_dialog == hwnd) g_dialog = nullptr;
		return TRUE;
	}
	}
	return FALSE;
}

static void ImportMediaDebug(EDIT_SECTION*) {
	if (g_dialog) {
		SetForegroundWindow(g_dialog);
		return;
	}

	auto* state = new MediaDialogState();
	DialogBuilder db;
	db.Begin(L"メディア読み込み(get_media_info)", 320, 140,
			 WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | DS_SETFONT | DS_MODALFRAME | WS_CLIPCHILDREN);
	db.AddControl(L"STATIC", L"ファイル", IDC_MEDIA_LABEL, 10, 10, 40, 12, SS_LEFT);
	db.AddControl(L"EDIT", L"", IDC_MEDIA_PATH, 60, 8, 200, 18, ES_READONLY | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
	db.AddControl(L"BUTTON", L"参照", IDC_MEDIA_BROWSE, 265, 8, 40, 18, BS_PUSHBUTTON);
	db.AddControl(L"STATIC", L"メディアを選択してください", IDC_MEDIA_STATUS, 10, 40, 295, 12, SS_LEFT);
	db.AddControl(L"BUTTON", L"読み込み", IDC_MEDIA_OK, 160, 80, 70, 24, BS_PUSHBUTTON);
	db.AddControl(L"BUTTON", L"キャンセル", IDC_MEDIA_CANCEL, 235, 80, 70, 24, BS_PUSHBUTTON);

	HWND dlg = CreateDialogIndirectParamW(GetModuleHandle(NULL), db.Get(), NULL, MediaDialogProc, (LPARAM)state);
	if (!dlg) {
		DWORD err = GetLastError();
		wchar_t buf[256]{};
		swprintf_s(buf, L"CreateDialogIndirectParamW failed: %lu", err);
		LogWarn(buf);
		delete state;
	} else {
		ShowWindow(dlg, SW_SHOW);
		SetForegroundWindow(dlg);
	}
}

// -----------------------------------------------------------------------------
// エントリーポイント
// -----------------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	if (!host) {
		return;
	}

	host->set_plugin_information(L"get_media_info Debug Loader v0.1");
	host->register_edit_menu(L"デバッグ\\メディア読み込み(get_media_info)2", ImportMediaDebug);
	g_edit = host->create_edit_handle();
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) {
	return true;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
	g_logger = handle;
	LogInfo(L"Logger initialized");
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	LogInfo(L"UninitializePlugin");
}
