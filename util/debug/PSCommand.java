import java.util.*;
import java.io.*;
import javax.swing.*;
import java.awt.*;
import java.awt.event.*;

import command.*;

public class PSCommand implements Command
{
	public String getName()
	{
		return "ps";
	}
	
	public String getHelp()
	{
		return "Render system state to a PostScript file, or standard output by default.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		final Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			try {
				final Process dot = Runtime.getRuntime().exec(new String[] {"dot", "-Tps"});
				
				/* We could just execute the code in run() below directly, but then we
				 * could block trying to write if dot writes back to us as we write to it.
				 * So, do the write in a separate thread spawned just for this purpose. */
				Runnable runnable = new Runnable()
				{
					public void run()
					{
						try {
							OutputStream out = dot.getOutputStream();
							dbg.render(new OutputStreamWriter(out), true);
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
				
				/* write the resulting PostScript file to its destination */
				InputStream input = dot.getInputStream();
				OutputStream output;
				if(args.length != 0)
					output = new FileOutputStream(new File(args[0]));
				else
					output = System.out;
				byte array[] = new byte[256];
				int bytes = input.read(array);
				while(bytes != -1)
				{
					output.write(array, 0, bytes);
					bytes = input.read(array);
				}
				output.close();
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
