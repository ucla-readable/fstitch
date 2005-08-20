import java.util.*;
import java.io.*;
import javax.swing.*;
import java.awt.*;
import java.awt.event.*;

import command.*;

public class ViewCommand implements Command
{
	private JFrame lastFrame = null;
	
	public String getName()
	{
		return "view";
	}
	
	public String getHelp()
	{
		return "View system state graphically, optionally in a new window.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		final Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			try {
				final Process dot = Runtime.getRuntime().exec(new String[] {"dot", "-Tgif"});
				
				/* We could just execute the code in run() below directly, but then we
				 * could block trying to write if dot writes back to us as we write to it.
				 * So, do the write in a separate thread spawned just for this purpose. */
				Runnable runnable = new Runnable()
				{
					public void run()
					{
						try {
							OutputStream out = dot.getOutputStream();
							dbg.render(new OutputStreamWriter(out), false);
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
				
				if(args.length > 0 && "new".equals(args[0]) && lastFrame != null)
				{
					final JFrame oldFrame = lastFrame;
					SwingUtilities.invokeLater(new Runnable()
					{
						public void run()
						{
							oldFrame.setTitle(oldFrame.getTitle().substring(2));
						}
					});
					lastFrame = null;
				}
				
				final ImageIcon icon = new ImageIcon(array);
				runnable = new Runnable()
				{
					public void run()
					{
						if(lastFrame == null)
						{
							final JFrame frame = new JFrame();
							frame.setDefaultCloseOperation(WindowConstants.DISPOSE_ON_CLOSE);
							frame.addWindowListener(new WindowAdapter()
							{
								public void windowClosed(WindowEvent e)
								{
									if(lastFrame == frame)
										lastFrame = null;
								}
							});
							lastFrame = frame;
						}
						
						JScrollPane image = new JScrollPane(new JLabel(icon));
						Container pane = lastFrame.getContentPane();
						
						pane.removeAll();
						lastFrame.setTitle("* " + dbg);
						pane.add(image);
						
						lastFrame.pack();
						lastFrame.setVisible(true);
					}
				};
				SwingUtilities.invokeLater(runnable);
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
