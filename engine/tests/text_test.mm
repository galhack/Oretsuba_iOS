// Text system smoke test: renders a font sample and a message window page.
//   text_test DATA_DIR OUT_DIR [--chs PATCH_DIR]
#import <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <string>

#include "core/engine.h"
#include "gfx/blend.h"
#include "platform/apple/apple_platform.h"
#include "stb_image_write.h"
#include "text/font.h"
#include "text/message.h"
#include "text/text_system.h"
#include "text/windows.h"
#include "ui/ui_system.h"

namespace {

std::string enc(const char *utf8, bool gbk) {
	CFStringRef s = CFStringCreateWithCString(nullptr, utf8, kCFStringEncodingUTF8);
	CFStringEncoding e = gbk ? kCFStringEncodingGB_18030_2000 : kCFStringEncodingDOSJapanese;
	CFIndex len = CFStringGetLength(s), need = 0;
	CFStringGetBytes(s, CFRangeMake(0, len), e, '?', false, nullptr, 0, &need);
	std::string out(size_t(need), '\0');
	CFStringGetBytes(s, CFRangeMake(0, len), e, '?', false, reinterpret_cast<UInt8 *>(&out[0]), need, nullptr);
	CFRelease(s);
	return out;
}

void write_png(const ore::Image &img, const std::string &path) {
	std::vector<uint8_t> rgba(size_t(img.width()) * img.height() * 4);
	for (int y = 0; y < img.height(); ++y)
		for (int x = 0; x < img.width(); ++x) {
			uint32_t p = img.row32(y)[x];
			uint8_t *d = &rgba[(size_t(y) * img.width() + x) * 4];
			d[0] = uint8_t(p >> 16), d[1] = uint8_t(p >> 8), d[2] = uint8_t(p), d[3] = 255;
		}
	stbi_write_png(path.c_str(), img.width(), img.height(), 4, rgba.data(), img.width() * 4);
}

void checker(ore::Image &img) {
	for (int y = 0; y < img.height(); ++y)
		for (int x = 0; x < img.width(); ++x)
			img.row32(y)[x] = ((x / 16 + y / 16) & 1) ? 0xff708090u : 0xffb0a080u;
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: text_test DATA OUT [--chs PATCH]\n");
		return 2;
	}
	std::string out = argv[2];
	bool chs = argc >= 5 && std::string(argv[3]) == "--chs";
	ore::ApplePlatform platform("/tmp/text_test_save");
	platform.set_chinese_fonts(chs);
	ore::EngineConfig cfg;
	cfg.data_dir = argv[1];
	if (chs) {
		cfg.patch_dir = argv[4];
		cfg.chinese = true;
	}
	ore::Engine e(platform, cfg);
	std::string err;
	if (!e.boot(&err)) {
		fprintf(stderr, "boot: %s\n", err.c_str());
		return 1;
	}
	bool gbk = e.encoding == ore::TextEncoding::Gbk;
	auto font = std::make_unique<ore::Font>(e, 22, enc("ＭＳ ゴシック", false), true);
	ore::Font &f = *font;
	uint32_t fh = f.handle();
	e.fonts.push_back(std::move(font));
	e.default_font = &f;

	// 1. font sample with the four looks
	ore::Image img(640, 200, 32);
	checker(img);
	std::string s1 = enc(gbk ? "「羽田君！」你好，世界。ABC abc 123" : "「羽田君！」自分が普通じゃない。ABC abc 123", gbk);
	f.draw(img, 10, 10, s1, true);
	f.effect = 2;
	f.draw(img, 10, 50, s1, true);
	f.effect = 1;
	f.shadow_dx = f.shadow_dy = 2;
	f.draw(img, 10, 90, s1, true);
	f.effect = 2;
	f.gradient = true;
	f.grad_top = 0xffeb00;
	f.grad_bottom = 0xff4646;
	f.draw(img, 10, 130, s1, true);
	f.gradient = false;
	int w, h, ox, oy;
	int lines = f.measure(s1, w, h, &ox, &oy);
	printf("measure: lines=%d w=%d h=%d off=%d,%d\n", lines, w, h, ox, oy);
	write_png(img, out + "/font.png");

	// 2. message window page as advinit sets it up
	auto mw = std::make_unique<ore::MessageWindow>(e, *e.ui, e.text->msg());
	if (mw->create(4, 466, 0, 0, fh, 0, "SYS/MS_FRM_B.ELG", "SYS/MS_FRM_A.ELG", "SYS/MS_FRM_G.ELG", 1500)) {
		fprintf(stderr, "window create failed\n");
		return 1;
	}
	mw->fullwidth = true, mw->char_spacing = 0, mw->line_spacing = 2, mw->autowrap = true, mw->wrap_reserve = 2;
	mw->update_wrap();
	mw->set_margins(28, 13, 16, 34, true);
	mw->set_pads(2, 2, 2, 2, true);
	auto nb = std::make_unique<ore::TextWindow>(e, *e.ui);
	nb->create(4, 427, 0, 0, fh, 0, "SYS/NM_FRM_B.ELG", "SYS/NM_FRM_A.ELG", "SYS/NM_FRM_G.ELG", 1500);
	nb->fullwidth = true;
	nb->autowrap = false;
	nb->set_margins(23, 7, 23, 7, true);
	nb->set_pads(2, 2, 2, 2, true);
	mw->name_box = nb->handle();
	mw->name_mode = 1;
	mw->set_bg_style(1, 0, 140);
	nb->set_bg_style(1, 0, 140);
	ore::MessageManager &mm = e.text->msg();
	mm.name = enc("鷹志", gbk) + "\\TAK1004";
	mm.body = enc(gbk ? "　我是普通人吗？这种事情，谁也不知道。不过，至少在这个城市里，我们都在寻找着属于自己的翅膀。今天也是平凡的一天，"
	                   "阳光很好，风也很温柔。"
	                 : "　自分が普通じゃないってことに気付いたのは、いつのことだったろう。<10>\\「ええっ！」<-10>"
	                   "それはきっと、ずっと前から分かっていたことだ。だけど誰にも言えなかった、そういうことってあるだろう？",
	              gbk) +
	          "$";
	mm.pos = 0;
	mw->show_instant();
	printf("voice=%s state=%d\n", mw->voice_id.c_str(), mw->state());
	ore::Image scr(800, 600, 32);
	checker(scr);
	ore::blit(scr, *nb->surface, nb->x(), nb->y(), nb->surface->rect(), 2, 255, false);
	ore::blit(scr, *mw->surface, mw->x(), mw->y(), mw->surface->rect(), 2, 255, false);
	write_png(scr, out + "/msgwin.png");

	// 3. reveal controller: type the first characters over a few ticks
	mm.pos = 0;
	mw->show(true);
	for (int t = 0; t < 12; ++t) e.tasks.tick(1000 + t * 17);
	printf("after ticks: state=%d pos=%zu\n", mw->state(), mm.pos);
	checker(scr);
	ore::blit(scr, *mw->surface, mw->x(), mw->y(), mw->surface->rect(), 2, 255, false);
	write_png(scr, out + "/reveal.png");

	// 4. select window
	auto sel = std::make_unique<ore::SelectWindow>(e, *e.ui);
	sel->create(38, 220, 0, 0, 0, fh, 1500);
	sel->set_images(1, false, false, "SYS/SEL01B.ELG", "SYS/SEL01A.ELG", "SYS/SEL01G.ELG", "SYS/SEL00B.ELG",
	                "SYS/SEL00A.ELG", "SYS/SEL00G.ELG", "SYS/SEL02B.ELG", "SYS/SEL02A.ELG", "SYS/SEL02G.ELG");
	sel->hl_mode = 1;
	sel->hl_color = 0x800000aa;
	sel->col_normal = 0xffffff, sel->col_chosen = 0x8fdfdf, sel->col_hover = 0xfff000;
	sel->ml = 20, sel->mr = 20, sel->tx = 2, sel->ty = 4;
	sel->set_bg_style(1, 0, 140);
	sel->add_item(enc(gbk ? "为了国家和民众" : "国と民衆のために", gbk).c_str(), false);
	sel->add_item(enc(gbk ? "为了明日香公主" : "アスカ姫のために", gbk).c_str(), false);
	sel->highlight(1, true);
	checker(scr);
	for (int i = -1; i < 2; ++i) {
		ore::Sprite *sp = dynamic_cast<ore::Sprite *>(sel->sprite(i));
		if (sp) sp->draw(scr, scr.rect());
	}
	write_png(scr, out + "/select.png");
	printf("ok\n");
	return 0;
}
