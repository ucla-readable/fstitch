public class InfoChdescLabel extends Opcode
{
	private final int chdesc;
	private final String label;
	
	public InfoChdescLabel(int chdesc, String label)
	{
		this.chdesc = chdesc;
		this.label = label;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.addLabel(label);
	}
	
	public String toString()
	{
		return "KDB_INFO_CHDESC_LABEL: chdesc = " + SystemState.hex(chdesc) + ", label = " + label;
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_INFO_CHDESC_LABEL, "KDB_INFO_CHDESC_LABEL", InfoChdescLabel.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("label", -1);
		return factory;
	}
}
