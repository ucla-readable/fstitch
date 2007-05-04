import java.io.*;
import java.util.*;

import command.*;

public class Debugger extends OpcodeFactory
{
	public final String name;
	
	private HashMap modules;
	private Vector opcodes;
	private SystemState state;
	private int applied;
	private boolean renderFree;
	private boolean renderBlock;
	private boolean renderOwner;
	private GrouperFactory grouperFactory;
	private int debugRev;
	private int debugOpcodeRev;
	
	public Debugger(String name, CountingDataInput input) throws BadInputException, IOException
	{
		super(input);
		this.name = name;
		modules = new HashMap();
		opcodes = new Vector();
		state = new SystemState(this);
		applied = 0;
		renderFree = false;
		renderBlock = true;
		renderOwner = true;
		grouperFactory = NoneGrouper.Factory.getFactory();
		
		debugRev = input.readInt();
		debugOpcodeRev = input.readInt();
		ensureSupportedRevision();
		
		addModule(new InfoModule(input));
		addModule(new BdescModule(input));
		addModule(new ChdescAlterModule(input));
		addModule(new ChdescInfoModule(input));
		addModule(new CacheModule(input));
		
		short module = input.readShort();
		if(module != 0)
			throw new UnexpectedModuleException(module, input.getOffset());
	}
	
	private void ensureSupportedRevision() throws UnsupportedStreamRevisionException
	{
		/* before the days of stream revision data */
		if(debugRev == 65536 && debugOpcodeRev == 1262764639)
			throw new UnsupportedStreamRevisionException(0, 0, 1288);
		
		/* known unsupported revisions */
		if(debugRev == 1290 && debugOpcodeRev == 1290)
			throw new UnsupportedStreamRevisionException(1290, 1290, 1577);
		if(debugRev == 1582 && debugOpcodeRev == 1577)
			throw new UnsupportedStreamRevisionException(1582, 1577, 1659);
		if(debugRev == 1660 && debugOpcodeRev == 1660)
			throw new UnsupportedStreamRevisionException(1660, 1660, 1662);
		if((debugRev == 1663 || debugRev == 1719 || debugRev == 1721 ||
		    debugRev == 1777 || debugRev == 1856 || debugRev == 1859 ||
		    debugRev == 1969) && debugOpcodeRev == 1663)
			throw new UnsupportedStreamRevisionException(debugRev, 1663, 1990);
		if(debugRev == 1991 && debugOpcodeRev == 1991)
			throw new UnsupportedStreamRevisionException(1991, 1991, 2001);
		if((debugRev == 2010 || debugRev == 2101 || debugRev == 2104) && debugOpcodeRev == 1991)
			throw new UnsupportedStreamRevisionException(debugRev, 1991, 2141);
		if(debugRev == 2004 && debugOpcodeRev == 2002) /* on a branch */
			throw new UnsupportedStreamRevisionException(2004, 2002, 2141);
		if(debugRev == 2142 && debugOpcodeRev == 1991)
			throw new UnsupportedStreamRevisionException(2142, 1991, 2583);
		if(debugRev == 2297 && debugOpcodeRev == 1991)
			throw new UnsupportedStreamRevisionException(2297, 1991, 2583);
		if(debugRev == 2584 && debugOpcodeRev == 2584)
			throw new UnsupportedStreamRevisionException(2584, 2584, 2692);
		if(debugRev == 2612 && debugOpcodeRev == 2584)
			throw new UnsupportedStreamRevisionException(2612, 2584, 2692);
		if(debugRev == 2693 && debugOpcodeRev == 2584)
			throw new UnsupportedStreamRevisionException(2693, 2584, 2702);
		if(debugRev == 2703 && debugOpcodeRev == 2703)
			throw new UnsupportedStreamRevisionException(2703, 2703, -1);
		if(debugRev == 2704 && debugOpcodeRev == 2703)
			throw new UnsupportedStreamRevisionException(2704, 2703, 2756);
		if(debugRev == 2747 && debugOpcodeRev == 2747)
			throw new UnsupportedStreamRevisionException(2747, 2747, 2756);
		if(debugRev == 2757 && debugOpcodeRev == 2757)
			throw new UnsupportedStreamRevisionException(2757, 2757, -1);
		if(debugRev == 2760 && debugOpcodeRev == 2760)
			throw new UnsupportedStreamRevisionException(2760, 2760, 2764);
		if(debugRev == 2765 && debugOpcodeRev == 2765)
			throw new UnsupportedStreamRevisionException(2765, 2765, 2765);
		if(debugRev == 2766 && debugOpcodeRev == 2766)
			throw new UnsupportedStreamRevisionException(2766, 2766, 2796);
		if(debugRev == 2786 && debugOpcodeRev == 2766)
			throw new UnsupportedStreamRevisionException(2786, 2766, 2796);
		if(debugRev == 2790 && debugOpcodeRev == 2766)
			throw new UnsupportedStreamRevisionException(2790, 2766, 2796);
		if(debugRev == 2796 && debugOpcodeRev == 2766)
			throw new UnsupportedStreamRevisionException(2796, 2766, 2796);
		if(debugRev == 2857 && (debugOpcodeRev == 2766 || debugOpcodeRev == 2810))
			throw new UnsupportedStreamRevisionException(2857, debugOpcodeRev, 2933);
		if((debugRev == 2934 || debugRev == 2953) && debugOpcodeRev == 2934)
			throw new UnsupportedStreamRevisionException(debugRev, 2934, 2970);
		if(debugRev == 2971 && debugOpcodeRev == 2934)
			throw new UnsupportedStreamRevisionException(2971, 2934, 3406); /* NOTE: flag change in r2972 */
		if((debugRev == 3017 || debugRev == 3103 || debugRev == 3123 ||
		    debugRev == 3330 || debugRev == 3371 || debugRev == 3379) && debugOpcodeRev == 2934)
			throw new UnsupportedStreamRevisionException(debugRev, 2934, 3406);
		if(debugRev == 3390 && (debugOpcodeRev == 3390 || debugOpcodeRev == 3396))
			throw new UnsupportedStreamRevisionException(3390, debugOpcodeRev, 3406);
		
		/* supported revisions */
		if(debugRev == 3390 && debugOpcodeRev == 3407)
			return;
		
		/* 0 means "use a newer revision" */
		throw new UnsupportedStreamRevisionException(debugRev, debugOpcodeRev, 0);
	}

	public void addModule(Module module)
	{
		//Short key = Short.valueOf(module.getModuleNumber());
		Short key = new Short(module.getModuleNumber());
		if(modules.containsKey(key))
			throw new RuntimeException("Duplicate module registered!");
		modules.put(key, module);
	}
	
	public Opcode readOpcode() throws BadInputException, IOException
	{
		/* get the opcode header (the source information) */
		String file = readString();
		int line = input.readInt();
		String function = readString();
		
		short number = input.readShort();
		//Short key = Short.valueOf(number);
		Short key = new Short(number);
		Module module = (Module) modules.get(key);
		if(module == null)
			throw new UnexpectedModuleException(number, input.getOffset());
		
		Opcode opcode = module.readOpcode();
		opcode.setFile(file);
		opcode.setLine(line);
		opcode.setFunction(function);
		
		/* get the opcode footer (the stack trace) */
		int address = input.readInt();
		UniqueStack.StackTemplate stack = new UniqueStack.StackTemplate();
		while(address != 0)
		{
			stack.addStackFrame(address);
			address = input.readInt();
		}
		opcode.setStack(stack.getUniqueStack());
		
		return opcode;
	}
	
	public int readOpcodes(long size) throws BadInputException, IOException
	{
		long nextPercent = 1;
		long nextOffset = size * nextPercent / 100;
		boolean star = false;
		try {
			for(;;)
			{
				opcodes.add(readOpcode());
				while(input.getOffset() >= nextOffset)
				{
					star = true;
					System.out.print("*");
					nextOffset = size * ++nextPercent / 100;
				}
			}
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
		finally
		{
			if(star)
				System.out.print(" ");
		}
		return opcodes.size();
	}
	
	public int readOpcodes(int count, long size) throws BadInputException, IOException
	{
		long nextPercent = 1;
		long nextOffset = size * nextPercent / 100;
		boolean star = false;
		try {
			while(count-- > 0)
			{
				opcodes.add(readOpcode());
				while(input.getOffset() >= nextOffset)
				{
					star = true;
					System.out.print("*");
					nextOffset = size * ++nextPercent / 100;
				}
			}
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
		finally
		{
			if(star)
				System.out.print(" ");
		}
		return opcodes.size();
	}
	
	public boolean replayAll()
	{
		boolean change = false;
		while(applied < opcodes.size())
		{
			Opcode opcode = (Opcode) opcodes.get(applied);
			try {
				opcode.applyTo(state);
			}
			catch(RuntimeException e)
			{
				System.out.println("interrupted! (" + applied + " opcodes OK)");
				throw e;
			}
			if(opcode.hasEffect())
				change = true;
			applied++;
		}
		return change;
	}
	
	public boolean replay(Opcode opcode)
	{
		opcode.applyTo(state);
		applied++;
		return opcode.hasEffect();
	}
	
	public boolean replay(int count)
	{
		boolean change = false;
		if(count == 1)
			while(applied < opcodes.size())
			{
				Opcode opcode = (Opcode) opcodes.get(applied++);
				opcode.applyTo(state);
				if(opcode.hasEffect())
					change = true;
				if(!opcode.isSkippable())
					break;
			}
		else
			while(applied < opcodes.size() && count-- > 0)
			{
				Opcode opcode = (Opcode) opcodes.get(applied++);
				opcode.applyTo(state);
				if(opcode.hasEffect())
					change = true;
			}
		return change;
	}
	
	public void resetState()
	{
		state = new SystemState(this);
		applied = 0;
	}
	
	public SystemState getState()
	{
		return state;
	}
	
	public int getApplied()
	{
		return applied;
	}
	
	public int getOpcodeCount()
	{
		return opcodes.size();
	}
	
	public Opcode getOpcode(int opcode)
	{
		return (Opcode) opcodes.get(opcode);
	}
	
	public boolean getRenderFree()
	{
		return renderFree;
	}
	
	public void setRenderFree(boolean renderFree)
	{
		this.renderFree = renderFree;
	}
	
	public boolean getRenderBlock()
	{
		return renderBlock;
	}
	
	public void setRenderBlock(boolean renderBlock)
	{
		this.renderBlock = renderBlock;
	}
	
	public boolean getRenderOwner()
	{
		return renderOwner;
	}
	
	public void setRenderOwner(boolean renderOwner)
	{
		this.renderOwner = renderOwner;
	}
	
	public GrouperFactory getGrouperFactory()
	{
		return grouperFactory;
	}
	
	public void setGrouperFactory(GrouperFactory grouperFactory)
	{
		this.grouperFactory = grouperFactory;
	}
	
	public void render(Writer output, boolean landscape) throws IOException
	{
		String title = "";
		if(applied > 0 && applied <= opcodes.size())
			title = opcodes.get(applied - 1).toString();
		state.render(output, title, renderFree, renderBlock, renderOwner, grouperFactory.newInstance(), landscape);
	}
	
	public String toString()
	{
		return "Debugging " + name + ", read " + opcodes.size() + " opcodes, applied " + applied;
	}
	
	public static void main(String args[])
	{
		if(args.length != 0 && args.length != 1 && args.length != 2)
		{
			System.err.println("Usage: java Debugger [file [count]]");
			return;
		}
		
		try {
			CommandInterpreter interpreter = new CommandInterpreter(new TwoCommandHistory());
			Debugger dbg = null;
			
			interpreter.addCommand(new CloseCommand());
			interpreter.addCommand(new GuiCommand());
			interpreter.addCommand(new JumpCommand());
			interpreter.addCommand(new ListCommand());
			interpreter.addCommand(new FindCommand());
			interpreter.addCommand(new LoadCommand());
			interpreter.addCommand(new StreamCommand());
			interpreter.addCommand(new OptionCommand());
			interpreter.addCommand(new PSCommand());
			interpreter.addCommand(new RenderCommand());
			interpreter.addCommand(new ResetCommand());
			interpreter.addCommand(new RunCommand());
			interpreter.addCommand(new StatusCommand());
			interpreter.addCommand(new StepCommand());
			interpreter.addCommand(new ViewCommand());
			
			/* "built-in" commands */
			interpreter.addCommand(new HelpCommand());
			interpreter.addCommand(new QuitCommand());
			
			if(args.length != 0)
			{
				String line = "load " + args[0];
				if(args.length != 1)
					line += " " + args[1];
				dbg = (Debugger) interpreter.runCommandLine(line, null, false);
			}
			
			interpreter.runStdinCommands("debug> ", dbg, true);
		}
		catch(IOException e)
		{
			System.err.println(e);
		}
		catch(CommandException e)
		{
			System.err.println(e);
		}
	}
}
