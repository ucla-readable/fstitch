import java.util.*;
import java.io.*;
import javax.swing.*;

import command.*;

public class ViewCommand implements Command
{
	public String getName()
	{
		return "view";
	}
	
	public String getHelp()
	{
		return "View system state graphically.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		final Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			try {
				final Process dot = Runtime.getRuntime().exec(new String[] {"dot", "-Tpng"});
				
				/* We could just execute the code in run() below directly, but then we
				 * could block trying to write if dot writes back to us as we write to it.
				 * So, do the write in a separate thread spawned just for this purpose. */
				Runnable runnable = new Runnable()
				{
					public void run()
					{
						try {
							OutputStream out = dot.getOutputStream();
							dbg.getState().render(new OutputStreamWriter(out), false);
							out.close();
						}
						catch(IOException e)
						{
						}
					}
				};
				Thread writer = new Thread(runnable);
				writer.setDaemon(true);
				writer.start();
				
				/* get the whole image as a byte array */
				InputStream input = dot.getInputStream();
				ByteArrayOutputStream stream = new ByteArrayOutputStream();
				byte array[] = new byte[256];
				int bytes = input.read(array);
				while(bytes != -1)
				{
					stream.write(array, 0, bytes);
					bytes = input.read(array);
				}
				array = stream.toByteArray();
				stream = null;
				
				/* this GUI code is not quite correct: you must make sure to close the
				 * previous image before trying to open a new one, or this code fails */
				ImageIcon icon = new ImageIcon(array);
				JFrame frame = new JFrame(dbg.toString());
				frame.setDefaultCloseOperation(WindowConstants.DISPOSE_ON_CLOSE);
				
				frame.getContentPane().add(new JLabel(icon));
				
				frame.pack();
				frame.setVisible(true);
			}
			catch(IOException e)
			{
				System.out.println("I/O error while creating image.");
			}
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
