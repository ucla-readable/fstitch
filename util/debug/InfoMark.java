public class InfoMark extends Opcode
{
	private final short module;
	
	public InfoMark(short module)
	{
		this.module = module;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_INFO_MARK: module = " + hex(module);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_INFO_MARK, "KDB_INFO_MARK", InfoMark.class);
		factory.addParameter("module", 2);
		return factory;
	}
}
