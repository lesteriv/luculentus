// Luculentus -- Proof of concept spectral path tracer
// Copyright (C) 2012, 2014  Ruud van Asseldonk
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "UserInterface.h"

using namespace Luculentus;

UserInterface::UserInterface(int argc, char** argv) :
  kit(argc, argv), // Initialises GTK
  window(),
  mainBox(false, 10),
  resultImage()
{
  // Create main window
  window.set_title("Luculentus");
  window.set_border_width(10);

  // Add the main vertical split, top part may contain controls in the
  // future, bottom part containing image
  window.add(mainBox);

  // The image that will display the render
  resultImage.set_alignment(0.5, 0.5);
  mainBox.pack_start(resultImage, true, true);

  // If the dispatcher is invoked, display the new image
  dispatcher.connect([this]() { this->DisplayImage(); });
}

void UserInterface::DisplayImage(int width, int height,
                                 const std::vector<std::uint8_t>& data)
{
  resultImageBuffer = Gdk::Pixbuf::create_from_data(
    &data[0],
    Gdk::COLORSPACE_RGB, // Use RGB colour
    false,               // No alpha channel
    8,                   // Eight bits per pixel (per channel)
    width,
    height,
    width * 3            // Rows have a width of three times the number
  );                     // of pixels (three channels, no unused gaps)

  // Call the dispatcher so it will update the UI
  dispatcher();
}

void UserInterface::DisplayImage()
{
  resultImage.set(resultImageBuffer);
}

void UserInterface::Run()
{
  window.show_all();
  Gtk::Main::run(window);
}
