import java.io.*;
import java.util.HashMap;
import java.util.Vector;

public class Debugger extends OpcodeFactory
{
	private HashMap modules;
	private Vector opcodes;
	
	public Debugger(DataInput input) throws BadInputException, IOException
	{
		super(input);
		modules = new HashMap();
		opcodes = new Vector();
		
		addModule(new BdescModule(input));
		addModule(new ChdescAlterModule(input));
		addModule(new ChdescInfoModule(input));
		
		short module = input.readShort();
		if(module != 0)
			throw new UnexpectedModuleException(module);
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
		String file = readString();
		int line = input.readInt();
		String function = readString();
		
		short number = input.readShort();
		//Short key = Short.valueOf(number);
		Short key = new Short(number);
		Module module = (Module) modules.get(key);
		if(module == null)
			throw new UnexpectedModuleException(number);
		
		Opcode opcode = module.readOpcode();
		opcode.setFile(file);
		opcode.setLine(line);
		opcode.setFunction(function);
		return opcode;
	}
	
	public void readOpcodes() throws BadInputException, IOException
	{
		try {
			for(;;)
				opcodes.add(readOpcode());
		}
		catch(EOFException e)
		{
			/* this is OK, we expect the end of the file sooner or later */
		}
	}
	
	public static void main(String args[])
	{
		if(args.length != 1)
		{
			System.out.println("Usage: java Debugger <file>");
			return;
		}
		
		try {
			File file = new File(args[0]);
			InputStream input = new FileInputStream(file);
			DataInput data = new DataInputStream(input);
			
			System.out.print("Reading debug signature... ");
			Debugger dbg = new Debugger(data);
			System.out.println("OK!");
			
			System.out.print("Reading debugging output... ");
			dbg.readOpcodes();
			System.out.println("OK!");
		}
		catch(BadInputException e)
		{
			System.out.println(e);
		}
		catch(EOFException e)
		{
			System.out.println("EOF!");
		}
		catch(IOException e)
		{
			System.out.println(e);
		}
	}
}
