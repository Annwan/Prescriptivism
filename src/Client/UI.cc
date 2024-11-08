module;
#include <algorithm>
#include <base/Assert.hh>
#include <cmath>
#include <numeric>
#include <pr/gl-headers.hh>
#include <ranges>
#include <SDL3/SDL.h>
#include <string_view>
#include <utility>
module pr.client.ui;

import base.text;
import pr.client.utils;
import pr.client.render;
import pr.client.render.gl;

using namespace pr;
using namespace pr::client;

// Define a setter that updates the property value if it is
// different, and, if so, also tells the element to refresh
// itself on the next frame.
#define TRIVIAL_CACHING_SETTER(class, type, property, ...) \
    void class ::set_##property(type new_value) {          \
        if (_##property == new_value) return;              \
        _##property = new_value;                           \
        needs_refresh = true;                              \
        __VA_ARGS__;                                       \
    }

// =============================================================================
//  Constants
// =============================================================================
constexpr Colour DefaultButtonColour{36, 36, 36, 255};
constexpr Colour HoverButtonColour{23, 23, 23, 255};

// =============================================================================
//  Helpers
// =============================================================================
auto Position::absolute(Size screen_size, Size object_size) -> xy {
    return relative(vec2(), screen_size, object_size);
}

auto Position::relative(AABB parent_box, Size object_size) -> xy {
    return relative(parent_box.origin(), parent_box.size(), object_size);
}

auto Position::relative(xy parent, Size parent_size, Size object_size) -> xy {
    static auto Clamp = [](i32 val, i32 obj_size, i32 total_size) -> i32 {
        if (val == Centered) return (total_size - obj_size) / 2;
        if (val < 0) return total_size + val - obj_size;
        return val;
    };

    auto [sx, sy] = parent_size;
    auto [obj_wd, obj_ht] = object_size;
    auto x = i32(parent.x) + Clamp(base.x, obj_wd, sx) + xadjust;
    auto y = i32(parent.y) + Clamp(base.y, obj_ht, sy) + yadjust;

    auto Adjust = [&](i32 xa, i32 ya) {
        if (base.x != Centered) x -= xa;
        if (base.y != Centered) y -= ya;
    };

    switch (anchor) {
        case Anchor::North: Adjust(obj_wd / 2, obj_ht); break;
        case Anchor::NorthEast: Adjust(obj_wd, obj_ht); break;
        case Anchor::East: Adjust(obj_wd, obj_ht / 2); break;
        case Anchor::SouthEast: Adjust(obj_wd, 0); break;
        case Anchor::South: Adjust(obj_wd / 2, 0); break;
        case Anchor::SouthWest: break;
        case Anchor::West: Adjust(0, obj_ht / 2); break;
        case Anchor::NorthWest: Adjust(0, obj_ht); break;
        case Anchor::Center: Adjust(obj_wd / 2, obj_ht / 2); break;
    }

    return {x, y};
}

// =============================================================================
//  Elements
// =============================================================================
void Button::draw(Renderer& r) {
    r.draw_rect(rbox(), hovered ? HoverButtonColour : DefaultButtonColour);
    TextBox::draw(r);
}

void Label::draw(Renderer& r) {
    auto& shaped = text.shaped(r);
    auto parent_box = parent->bounding_box;
    auto position = auto{pos}.voffset(i32(shaped.depth)).relative( //
        parent_box,
        shaped.size()
    );

    r.draw_text(shaped, position, colour);
}

void Label::refresh(Renderer& r) {
    if (not reflow) return;
    text.reflow(r, parent->bounding_box.width());
}

TextBox::TextBox(
    Element* parent,
    ShapedText text,
    ShapedText placeholder,
    Position pos,
    i32 padding,
    i32 min_wd,
    i32 min_ht
) : Widget{parent, pos},
    placeholder{std::move(placeholder)},
    padding{padding},
    min_wd{min_wd},
    min_ht{min_ht} {
    UpdateText(std::move(text));
}

void TextBox::UpdateText(ShapedText new_text) {
    label = std::move(new_text);
    needs_refresh = true;
}

auto TextBox::TextPos(const ShapedText& text) -> xy {
    return Position::Center().voffset(i32(text.depth)).relative(rbox(), text.size());
}

void TextBox::draw(Renderer& r) {
    auto& text = label.empty() ? placeholder : label;
    auto pos = TextPos(text);
    r.draw_text(text, pos, label.empty() ? Colour::Grey : Colour::White);
    if (cursor_offs != -1) {
        auto [asc, desc] = r.font_for_text(label).strut_split();
        r.draw_line(
            xy(i32(pos.x) + cursor_offs, pos.y - i32(desc)),
            xy(i32(pos.x) + cursor_offs, pos.y + i32(asc)),
            Colour::White
        );
    }
}

void TextBox::refresh(Renderer& r) {
    auto strut = r.font_for_text(label).strut();
    Size sz{
        std::max(min_wd, i32(label.width)) + 2 * padding,
        std::max({min_ht, i32(label.height + label.depth), strut}) + 2 * padding,
    };

    SetBoundingBox(rpos(), sz);
}

void TextEdit::draw(Renderer& r) {
    if (dirty) {
        dirty = false;
        auto shaped = r.make_text(
            hide_text ? std::u32string(text.size(), U'•') : text,
            size,
            style,
            TextAlign::SingleLine,
            0,
            &clusters
        );

        UpdateText(std::move(shaped));
    }

    // Use HarfBuzz cluster information to position the cursor: if the cursor
    // position corresponds to an entry in the clusters array (e.g. the cursor
    // is at position 3, and the array has an entry with value 3), position the
    // cursor right before the character that starts at that cluster (i.e. the
    // n-th character, where n is the index of the entry with value 3).
    //
    // If there is no entry, find the smallest cluster value 's' closest to the
    // cursor position, take the difference between it and the next cluster, and
    // lerp the cursor in the middle of the character at 's'.
    //
    // Concretely, there are 3 possible cases here:
    //
    //   1. The cursor is at the start/end of the text, in which case it has
    //      index 0/n (where n is the number of characters in the text); draw
    //      it before the first / after the last character.
    //
    //   2. The cursor corresponds to the index of a cluster; draw it before
    //      that cluster.
    //
    //   3. The cursor corresponds to an index that is in between two clusters;
    //      interpolate between them to position the cluster in the middle
    //      somewhere.
    if (no_blink_ticks) no_blink_ticks--;
    if (selected and not clusters.empty() and (no_blink_ticks or r.blink_cursor())) {
        cursor_offs = [&] -> i32 {
            // Cursor is at the start/end of the text.
            if (cursor == 0) return 0;
            if (cursor == i32(text.size())) return i32(label.width);

            // Find the smallest cluster with an index greater than or equal
            // to the cursor position. We interpolate the cursor’s position
            // between it and the previous cluster.
            //
            // Note that there *must* always be a cluster with an index *smaller*
            // than the cursor, since there will always be a cluster with index
            // 0, and the cursor index cannot be zero (because we checked for that
            // above).
            auto it = rgs::lower_bound(clusters, cursor, {}, &ShapedText::Cluster::index);
            auto prev = std::prev(it);
            i32 x1 = prev->xoffs;
            i32 i1 = prev->index;
            i32 x2, i2;

            // If we get here, the cursor must not be at the very start or end
            // of the text; despite that, there may not be a cluster with an
            // index larger than the cursor’s.
            //
            // This can happen e.g. if we have a ligature at the end of the text.
            // For instance, if the text is 'fl', which is converted to a single
            // ligature, the clusters array contains a single cluster with index 0.
            //
            // In this case, cursor index 1—despite being larger than the index of
            // any cluster—is *not* at the end of the text. However, we can resolve
            // this by pretending there is an additional cluster at the end of the
            // array whose index is the size of the text, and then interpolate between
            // that and the last actual cluster.
            if (it != clusters.end()) {
                // Cursor is right before a character.
                if (it->index == cursor) return i32(it->xoffs);

                // Cursor is between two clusters.
                x2 = it->xoffs;
                i2 = it->index;
            }

            // Interpolate between the last cluster and the end of the text.
            else {
                x2 = i32(label.width);
                i2 = i32(text.size());
            }

            return i32(std::lerp(x1, x2, f32(cursor - i1) / f32(i2 - i1)));
        }();
    } else {
        cursor_offs = -1;
    }

    if (hovered) r.set_cursor(Cursor::IBeam);

    r.draw_rect(rbox(), selected ? HoverButtonColour : DefaultButtonColour);
    TextBox::draw(r);
}

void TextEdit::event_click(InputSystem& input) {
    // Figure out where we clicked and set the cursor accordingly;
    // we do this by iterating over all clusters; as soon as we find
    // one whose offset brings us further away from the click position,
    // we stop and go back to the one before it.
    no_blink_ticks = 20;
    i32 mx = input.mouse.pos.x;
    i32 x0 = TextPos(label).x;
    i32 x1 = x0 + i32(label.width);
    if (mx < x0) cursor = 0;
    else if (mx > x1) cursor = i32(text.size());
    else if (clusters.size() < 2) cursor = 0;
    else {
        cursor = 0;
        i32 x = x0;
        i32 d = std::abs(x - mx);
        auto it = clusters.begin();

        // A cluster might correspond to multiple glyphs, in which case
        // we need to interpolate into it.
        ShapedText::Cluster* prev = nullptr;
        while (cursor < i32(text.size()) and it != clusters.end()) {
            auto xoffs = [&] {
                // Cluster matches cursor index; we can use the x
                // offset exactly.
                if (cursor == it->index) return it->xoffs;

                // Cluster index is too large; interpolate between the
                // previous index and this one.
                auto prev_x = prev ? prev->xoffs : 0;
                auto prev_i = prev ? prev->index : 0;
                return i32(std::lerp(prev_x, it->xoffs, f32(cursor - prev_i) / f32(it->index - prev_i)));
            }();

            auto nd = std::abs(x + xoffs - mx);
            if (nd > d) {
                cursor--;
                break;
            }

            d = nd;
            prev = &*it;
            cursor++;
            if (cursor > it->index) ++it;
        }

        cursor = std::clamp(cursor, 0, i32(text.size()));
    }
}

void TextEdit::event_input(InputSystem& input) {
    // Copy text into the buffer.
    if (not input.text_input.empty()) {
        no_blink_ticks = 20;
        dirty = true;
        text.insert(cursor, input.text_input);
        cursor += i32(input.text_input.size());
    }

    auto Paste = [&] {
        if (SDL_HasClipboardText()) {
            text += text::ToUTF32(SDL_GetClipboardText());
            dirty = true;
        }
    };

    for (auto [key, mod] : input.kb_events) {
        no_blink_ticks = 20;
        switch (key) {
            default: break;
            case SDLK_BACKSPACE:
                if (mod & SDL_KMOD_CTRL) {
                    static constexpr std::u32string_view ws = U" \t\n\r\v\f";
                    u32stream segment{text.data(), usz(cursor)};
                    auto pos = segment.trim_back().drop_back_until_any(ws).size();
                    text.erase(pos, cursor - pos);
                    cursor = i32(pos);
                    dirty = true;
                } else if (cursor != 0) {
                    text.erase(--cursor, 1);
                    dirty = true;
                }
                break;
            case SDLK_DELETE:
                if (cursor != i32(text.size())) {
                    text.erase(cursor, 1);
                    dirty = true;
                }
                break;
            case SDLK_LEFT: cursor = std::max(0, cursor - 1); break;
            case SDLK_RIGHT: cursor = std::min(i32(text.size()), cursor + 1); break;
            case SDLK_HOME: cursor = 0; break;
            case SDLK_END: cursor = i32(text.size()); break;
            case SDLK_V:
                if (mod & SDL_KMOD_CTRL) Paste();
                break;
            case SDLK_INSERT:
                if (mod & SDL_KMOD_SHIFT) Paste();
                break;
        }
    }
}

Throbber::Throbber(Element* parent, Position pos)
    : Widget(parent, pos), vao(VertexLayout::Position2D) {
    vec2 verts[]{
        {-R, -R},
        {-R, R},
        {R, -R},
        {R, R}
    };
    vao.add_buffer(verts, gl::GL_TRIANGLE_STRIP);
    UpdateBoundingBox(Size{i32(R), i32(R)});
}

void Throbber::draw(Renderer& r) {
    static constexpr f32 Rate = 3; // Smaller means faster.

    // Uses absolute position because it may not have a parent.
    auto at = pos.absolute(r.size(), {i32(R), i32(R)});
    auto rads = f32(glm::radians(fmod(360 * Rate - SDL_GetTicks(), 360 * Rate) / Rate));
    auto xfrm = glm::identity<mat4>();
    xfrm = translate(xfrm, vec3(R, R, 0));
    xfrm = rotate(xfrm, rads, vec3(0, 0, 1));

    r.use(r.throbber_shader);
    r.throbber_shader.uniform("position", at.vec());
    r.throbber_shader.uniform("rotation", xfrm);
    r.throbber_shader.uniform("r", R);

    vao.draw_vertices();
}

Card::Card(
    Element* parent,
    Position pos
) : Widget{parent, pos},
    _id{CardId::$$Count},
    _scale{Field},
    code{this, Position()},
    name{this, Position()},
    middle{this, Position::Center()},
    special{this, Position()} {
    code.colour = Colour::Black;
    name.colour = Colour::Black;
    middle.colour = Colour::Black;
    special.colour = Colour::Black;
}

void Card::draw(Renderer& r) {
    auto offs = Offset[scale];
    auto sz = CardSize[scale];
    auto at = pos.relative(parent->bounding_box, sz);

    r.draw_rect(at, sz);
    if (selected) r.draw_outline_rect(
        at,
        sz,
        CardGroup::CardGaps[scale] / 2,
        Colour{50, 50, 200, 255}
    );

    code.draw(r);
    middle.draw(r);
    special.draw(r);
    name.draw(r);

    for (int i = 0; i < count; ++i) r.draw_rect(
        Position{-3 * offs, -(2 * offs + 2 * i * offs)}.relative(at, sz, {5 * offs, offs}),
        {5 * offs, offs},
        Colour::Black
    );
}

void Card::refresh(Renderer& r) {
    SetBoundingBox(AABB{pos.relative(parent->bounding_box, CardSize[scale]), CardSize[scale]});
    if (not scale_changed) return;
    scale_changed = false;

    // Adjust label font sizes.
    code.font_size(CodeSizes[scale]);
    name.font_size(NameSpecialSizes[scale]);
    middle.font_size(MiddleSizes[scale]);
    special.font_size(NameSpecialSizes[scale]);

    // Adjust label positions.
    code.pos = Position{Offset[scale], -Offset[scale]};
    special.pos = Position::HCenter(10 * Offset[scale]);
    name.pos = Position(Offset[scale], -(4 * Offset[scale] + code.size(r).ht));
}

void Card::set_id(CardId ct) {
    if (ct == CardId::$$Count) return;
    _id = ct;
    auto& data = CardDatabase[+ct];
    if (data.type == CardType::SoundCard) {
        count = std::saturate_cast<u8>(data.count_in_deck);
        code.update_text(std::format( //
            "{}{}{}{}",
            data.is_consonant() ? 'P' : 'F',
            data.place_or_frontness,
            data.is_consonant() ? 'M' : 'H',
            data.manner_or_height
        ));

        name.update_text(std::string{data.name});
        middle.update_text(std::string{data.center});
        special.update_text( // clang-format off
            utils::join(data.converts_to | vws::transform([](auto& vec) {
                return std::format("→ {}", utils::join(vec | vws::transform([](CardId id) {
                    return CardDatabase[+id].center;
                }), ", "));
            }), "\n")
        ); // clang-format on
        needs_refresh = true;
    } else {
        Log("TODO: Power cards");
    }
}

TRIVIAL_CACHING_SETTER(Card, Scale, scale, scale_changed = true)

void CardGroup::draw(Renderer& r) {
    for (auto& card : cards) card->draw(r);
}

void CardGroup::refresh(Renderer& r) {
    if (cards.empty()) return;

    // If we’re allowed to scale up, determine the maximum scale that works.
    Scale s;
    i32 width = max_width != 0 ? max_width : bounding_box.size().wd;
    if (autoscale) {
        s = Scale(Scale::NumScales - 1);
        while (s != scale) {
            auto wd = i32(cards.size() * Card::CardSize[s].wd + (cards.size() - 1) * CardGaps[s]);
            if (wd < width) break;
            s = Scale(s - 1);
        }
    } else {
        s = scale;
    }

    i32 x = 0;
    for (auto& c : cards) {
        c->scale = s;
        c->pos = Position::VCenter(x);
        x += Card::CardSize[s].wd + CardGaps[s];
    }

    auto sz = Size{x - CardGaps[s], Card::CardSize[s].ht};
    SetBoundingBox(AABB{pos.relative(parent->bounding_box, sz), sz});
    for (auto& c : cards) c->refresh(r);
}

void CardGroup::add(CardId c) {
    cards.push_back(std::make_unique<Card>(this, Position()));
    cards.back()->id = c;
    needs_refresh = true;
}

TRIVIAL_CACHING_SETTER(CardGroup, bool, autoscale);
TRIVIAL_CACHING_SETTER(CardGroup, i32, max_width);
TRIVIAL_CACHING_SETTER(CardGroup, Scale, scale);

// =============================================================================
//  Input Handler.
// =============================================================================
void InputSystem::process_events() {
    kb_events.clear();
    text_input.clear();

    // Get mouse state.
    mouse = {};
    f32 x, y;
    SDL_GetMouseState(&x, &y);
    mouse.pos = {x, renderer.size().ht - y};

    // Process events.
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            default: break;
            case SDL_EVENT_QUIT:
                quit = true;
                break;

            // Record the button presses instead of acting on them immediately; this
            // has the effect of debouncing clicks within a single tick.
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) mouse.left = true;
                if (event.button.button == SDL_BUTTON_RIGHT) mouse.right = true;
                if (event.button.button == SDL_BUTTON_MIDDLE) mouse.middle = true;
                break;

            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_F12) renderer.reload_shaders();
                kb_events.emplace_back(event.key.key, event.key.mod);
                break;

            case SDL_EVENT_TEXT_INPUT:
                text_input += text::ToUTF32(event.text.text);
                break;
        }
    }
}

void InputSystem::update_selection(bool is_element_selected) {
    if (was_selected == is_element_selected) return;
    was_selected = is_element_selected;
    if (is_element_selected) SDL_StartTextInput(renderer.sdl_window());
    else SDL_StopTextInput(renderer.sdl_window());
}

// =============================================================================
//  Screen
// =============================================================================
void Screen::draw(Renderer& r) {
    r.set_cursor(Cursor::Default);
    for (auto& e : visible()) e->draw(r);
}

void Screen::refresh(Renderer& r) {
    SetBoundingBox(AABB({0, 0}, r.size()));

    // Size hasn’t changed. Still update any elements that
    // requested a refresh. Also ignore visibility here.
    if (prev_size == r.size()) {
        for (auto& e : children) {
            if (e->needs_refresh) {
                e->needs_refresh = false;
                e->refresh(r);
            }
        }

        return;
    }

    // Refresh every visible element, and every element that
    // requested a refresh.
    prev_size = r.size();
    for (auto& e : children) {
        if (e->visible or e->needs_refresh) {
            e->needs_refresh = false;
            e->refresh(r);
        }
    }
}

void Screen::tick(InputSystem& input) {
    // Deselect the currently selected element if there was a click.
    if (input.mouse.left) selected_element = nullptr;

    // Tick each child.
    for (auto& e : visible()) {
        // First, reset all of the child’s properties so we can
        // recompute them.
        e->reset_properties();

        // If the cursor is within the element’s bounds, mark it as hovered.
        e->hovered = e->bounding_box.contains(input.mouse.pos);

        // If, additionally, we had a click, select the element and fire the
        // event handler.
        if (e->hovered and input.mouse.left) {
            if (e->selectable) selected_element = e.get();
            e->event_click(input);
        }
    }

    // Mark the selected element as selected once more.
    if (selected_element) {
        selected_element->selected = true;
        selected_element->event_input(input);
    }

    // In any case, tell the input system whether we have a
    // selected element.
    input.update_selection(selected_element != nullptr);
}

// =============================================================================
//  Game Loop.
// =============================================================================
void InputSystem::game_loop(std::function<void()> tick) {
    constexpr chr::milliseconds ClientTickDuration = 16ms;
    while (not quit) {
        auto start_of_tick = chr::system_clock::now();

        // Handle user input.
        process_events();

        tick();

        const auto end_of_tick = chr::system_clock::now();
        const auto tick_duration = chr::duration_cast<chr::milliseconds>(end_of_tick - start_of_tick);
        if (tick_duration < ClientTickDuration) {
            SDL_WaitEventTimeout(
                nullptr,
                i32((ClientTickDuration - tick_duration).count())
            );
        } else {
            Log("Client tick took too long: {}ms", tick_duration.count());
        }
    }
}
